// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * It has these expected usage modes:
 *
 * 1 - No cache, device stops suddenly.
 *
 *     There is no cache.  At a point chosen by the test, we suddenly start
 *     failing all writes with an EIO.
 *
 * 2 - There is a cache of 4K blocks.  The device obeys proper REQ_FLUSH and
 *     REQ_FUA semantics.  At a point chosen by the test, we suddenly start
 *     failing all writes with an EIO, and forget to write the contents of the
 *     write cache.
 *
 *     The cache is not managed to improve performance or reliability, but
 *     merely provides data that we forget to write.
 *
 *     The cache size can be large or small, which determines the size of the
 *     disruption caused by the device failure.
 *
 * 3 - There is a cache of 512 byte blocks (sectors).  The device obeys proper
 *     REQ_FLUSH and REQ_FUA semantics.  At a point chosen by the test, we
 *     suddenly start failing all writes with an EIO, and forget to write the
 *     contents of the write cache.
 *
 *     We do not cache every sector, but select which sectors to cache so as to
 *     produce torn writes when we stop the device.  We use a modulus and mask
 *     to decide with sectors to cache.  Specifically, we cache a sector when
 *     this expression evaluates to a true value:
 *
 *          mask & (1 << (sector_number % modulus))
 *
 *     Using modulus of 8 with a mask with only 1 bit set will cache only 1
 *     sector of a 4K block and will cause the lossy device to fail to write
 *     that sector.  Using modulus of 8 with a mask with only 1 bit clear will
 *     cache all but 1 sector of a 4K block, and will cause the lossy device to
 *     write only 1 sector of the block.
 *
 *     A more interesting effect happens with a modulus of 9.  Similar mask
 *     settings will result in the sector that is/isn't written to change to a
 *     different offset in each 4K block.
 */

#include <linux/blk_types.h>
#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <uapi/linux/dm-ioctl.h>
#ifndef VDO_UPSTREAM

// only needed for tests; see dm-vdo-target.c
#include "dm-lossy-target.h"
#endif /* VDO_UPSTREAM */

#define MAX_CACHE_BLOCKS 0xFFEC
#define MAX_MASK_LENGTH 64
#define LOSSY_NAME_SIZE 12
#define DM_MSG_PREFIX "lossy"

/*
 * Cache block states.  Note that all state changes are protected by a spin
 * lock.  The states are:
 *
 *   EMPTY    The cache block is not used and is available.
 *
 *   COPYING  There is an active bio doing a read or write to the cache block.
 *
 *   DIRTY    The cache block is in use, but there is no active I/O on it.
 *
 *   WRITING  The cache block is being written to storage.
 *
 * This driver intends to be correct until it is told to stop doing any writing
 * to storage.  It sometimes prefers to be simple rather than fast.  These are
 * the state transitions that it performs:
 *
 *   EMPTY => COPYING => DIRTY
 *
 *     This transition takes a cache block from unused to used.  We only do
 *     this for an ordinary write of a full block.  This means the "copying"
 *     copies a full block from the I/O request into the cache, and ensures
 *     that there are no partial blocks in the cache.
 *
 *   DIRTY => COPYING => DIRTY
 *
 *     This transition services an I/O request using the cache block.
 *
 *   DIRTY => WRITING => EMPTY
 *
 *     This transition writes the cache block to storage.  It can occur
 *     when an empty REQ_FLUSH request is being processed, or when a write
 *     request to this block is either an REQ_DISCARD or REQ_FUA request.
 *     When the write completes, the cache block returns to EMPTY state.
 *     We do not try to maintain any "clean" blocks in the cache.
 */
enum __attribute__((__packed__)) block_state {
  EMPTY,
  COPYING,
  DIRTY,
  WRITING,
};

struct lossy_device;

struct cache_block {
  // A spin lock that protects the cache block.  It is taken by the bi_end_io
  // callback when we write a cache block, and therefore should be used with
  // spin_lock_irq or spin_lock_irqsave.
  spinlock_t               lock;
  // When this block is in COPYING or WRITING state, bios that refer to this
  // block are put on this list and processed later.
  struct bio_list          pending_bios;
  // Pointer back to the struct lossy_device containing this block.
  struct lossy_device     *device;
  // Pointer to the data for this block.
  char                    *data;
  // Pointer to the bio reserved for use when we need to write this block.
  struct bio              *bio;
  // The BLOCK number of this block (not the sector number).
  sector_t                 block_number;
  // The state of this cache block
  enum block_state         state;
};

struct lossy_device {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev  *dev;
  // Return value for unsuccessful writes.
  blk_status_t    error_status;
  // Flag that is set to true to stop all writes by the device.
  bool            stopped;
  // The name of the device.
  char            name[LOSSY_NAME_SIZE + 1];
  // Pointer to the cached data, used only for allocate/free of the memory.
  char           *cache_data;
  // The block size, which must be either 512 or 4K.
  size_t          block_size;
  // Settings for producing torn writes.
  u32             block_mask;
  u32             mask_length;
  // The block shift, which is used to convert sector numbers to block numbers.
  // Will be either 0 (for block size 512) or 3 (for block size 4K).
  u16             block_shift;
  // The number of cache blocks, which may be zero for no block cache.
  u16             cache_block_count;
  // The busy count of the device, which is used to implement proper REQ_FLUSH
  // requests when there is a block cache.  It counts the number of bios that
  // we are actively working on, and the number of dirty blocks in the block
  // cache.  An REQ_FLUSH request cannot be completed until this count goes to
  // zero.
  atomic_t        busy_count;

  // BEGIN data that pertains to work done in a kworker thread for this
  // device.  This spin lock protects these data, and it is taken by the
  // bi_end_io callback when we write a cache block, and therefore should be
  // used with spin_lock_irq or spin_lock_irqsave.
  spinlock_t         work_lock;
  // When the processing of a bio has been delayed, it will eventually be put
  // on this list and processed in a kworker thread.
  struct bio_list    work_bios;
  // When the processing of an REQ_FLUSH request has been completed, it will be
  // put on this list and processed in a kworker thread.
  struct bio_list    work_flush_bios;
  // This is a Linux work item used to schedule processing of the work_bios
  // list.
  struct work_struct work_item;
  // END of data protected by work_lock.

  // BEGIN data that pertains to processing REQ_FLUSH requests.  This spin lock
  // protects these data, and may be taken by the bi_end_io callback when we
  // write a cache block, and therefore should be used with spin_lock_irq or
  // spin_lock_irqsave.
  spinlock_t      flush_lock;
  // A flag to indicate that a flush is in progress.
  bool            flushing;
  // When an REQ_FLUSH bio arrives, it will be put onto this list for
  // processing at the proper time.
  struct bio_list flush_bios;
  // While flushing, all non-REQ_FLUSH bios are put onto this list for
  // processing when the flush is completed.
  struct bio_list pending_bios;
  // END of data protected by flush_lock.

  // BEGIN data that are merely statistics and do not effect code behavior.
  // These stats count the bios that arrive into the lossy_map method.
  atomic64_t    total_reads;
  atomic64_t    total_writes;
  atomic64_t    total_flushes;
  atomic64_t    total_fuas;
  atomic64_t    write_failures;
  atomic64_t    flush_failures;
  unsigned long reads_at_last_flush;
  unsigned long writes_at_last_flush;
  unsigned long reads_at_stop;
  unsigned long writes_at_stop;
  // These stats count the values returned by the lossy_map method.
  atomic64_t    mapped_returns;
  atomic64_t    submitted_returns;
  // These stats count the bios for which the lossy_map method returned
  // "submitted".
  atomic64_t    submitted_bios;
  atomic64_t    success_bios;
  atomic64_t    error_bios;
  // END of statistics

  // The block cache (variable sized, so it goes at the end).
  struct cache_block cache_blocks[];
};

/**********************************************************************/
// BEGIN large section of code for the block cache
/**********************************************************************/

static void process_ready_bios(struct lossy_device *ld, struct bio_list *ready);

/**
 * Do delayed processing of a list of bios in a kworker thread.
 *
 * @param work  A kworker work struct.
 **/
static void process_pending_bios(struct work_struct *work)
{
  struct lossy_device *ld = container_of(work, struct lossy_device, work_item);
  struct bio_list flushes;
  struct bio_list ready;
  struct bio *bio;

  // Under the worklock, grab the lists of bios to be processed.
  bio_list_init(&flushes);
  bio_list_init(&ready);
  spin_lock_irq(&ld->work_lock);
  bio_list_merge_init(&flushes, &ld->work_flush_bios);
  bio_list_merge_init(&ready, &ld->work_bios);
  spin_unlock_irq(&ld->work_lock);

  // Process the completed flushes.
  while ((bio = bio_list_pop(&flushes))) {
    if (ld->stopped && (atomic64_read(&ld->write_failures) > 0)) {
      // We are stopping writes and failed to write a cached block.
      bio->bi_status = ld->error_status;
      bio_endio(bio);
      atomic64_inc(&ld->error_bios);
    } else {
      // Still succeeding, so forward the flush to the storage medium.
      submit_bio_noacct(bio);
      atomic64_inc(&ld->submitted_bios);
    }
  }

  // Process the delayed bios.
  process_ready_bios(ld, &ready);
}

/**********************************************************************/
/**
 * Schedule delayed processing of bios.  This uses the Linux kworker
 * threads, so as to avoid extended processing in a bi_end_io callback.
 *
 * @param ld       The lossy device
 * @param ready    bio list of bios that are now unblocked
 * @param flushes  bio list of REQ_FLUSH bios that are now complete
 **/
static void schedule_pending_bios(struct lossy_device *ld,
                                  struct bio_list     *ready,
                                  struct bio_list     *flushes)
{
  bool new_bios = !bio_list_empty(ready);
  bool new_flushes = flushes && !bio_list_empty(flushes);
  bool schedule_work_item;
  unsigned long flags;

  // If the lists of new bios are empty, there is nothing to do.
  if (!new_flushes && !new_bios)
    return;

  // Under the worklock, add the new bios to the existing lists of bios to
  // process.
  spin_lock_irqsave(&ld->work_lock, flags);
  schedule_work_item = (bio_list_empty(&ld->work_bios)
                        && bio_list_empty(&ld->work_flush_bios));
  if (new_bios)
    bio_list_merge_init(&ld->work_bios, ready);
  if (new_flushes)
    bio_list_merge_init(&ld->work_flush_bios, flushes);
  spin_unlock_irqrestore(&ld->work_lock, flags);

  // If we added to empty lists, schedule a work item.  Otherwise there is
  // already a work item scheduled.
  if (schedule_work_item) {
    INIT_WORK(&ld->work_item, process_pending_bios);
    schedule_work(&ld->work_item);
  }
}

/**********************************************************************/
/**
 * Decrement the busy count.  If it goes to zero and a flush is in progress,
 * finish the flush.  This method can be called from a bi_end_io callback.
 *
 * @param ld  The lossy device
 **/
static void complete_flushes(struct lossy_device *ld)
{
  struct bio_list completed_flushes;
  struct bio_list ready_bios;
  unsigned long flags;

  if (atomic_dec_and_test(&ld->busy_count)) {
    // The busy count has just dropped to zero, so we need to take flush_lock
    // and deal with any flushes in progress.
    bio_list_init(&completed_flushes);
    bio_list_init(&ready_bios);
    spin_lock_irqsave(&ld->flush_lock, flags);
    if (ld->flushing) {
      // And there are REQ_FLUSH requests in progress.
      ld->flushing = false;
      // Record the flush bios that are complete.
      bio_list_merge_init(&completed_flushes, &ld->flush_bios);
      // Record the bios that are now ready to start.
      bio_list_merge_init(&ready_bios, &ld->pending_bios);
     }
    spin_unlock_irqrestore(&ld->flush_lock, flags);

    // Start the "ready" ones.
    schedule_pending_bios(ld, &ready_bios, &completed_flushes);
  }
}

/**********************************************************************/
/**
 * bi_end_io callback routine for when a cache block write completes
 *
 * @param bio    The bio
 **/
static void end_flush_cache_block(struct bio *bio)
{
  int result = blk_status_to_errno(bio->bi_status);
  struct cache_block *cb = bio->bi_private;
  struct lossy_device *ld = cb->device;
  struct bio_list ready;
  unsigned long flags;

  if (result)
    DMWARN("error flushing at sector %llu: %d\n",
           (unsigned long long) (cb->block_number << ld->block_shift), result);

  bio_list_init(&ready);

  // Set the block state to EMPTY.  This is a transition from WRITING to EMPTY.
  spin_lock_irqsave(&cb->lock, flags);
  cb->state = EMPTY;
  // Record the bios that are now ready to start
  bio_list_merge_init(&ready, &cb->pending_bios);
  spin_unlock_irqrestore(&cb->lock, flags);

  // Finish the transition to EMPTY.
  complete_flushes(ld);

  // Start any bios that were waiting for this specific cache block.
  schedule_pending_bios(ld, &ready, NULL);
}

/**********************************************************************/
/**
 * Flush a cache block to storage.
 *
 * @param cb  The cache block (locked)
 **/
static void flush_cache_block(struct cache_block *cb)
{
  struct lossy_device *ld = cb->device;
  int bytes_added;

  // Set the block state to WRITING, and release the cache block lock.  We do
  // not want to hold the lock while we write the data.
  cb->state = WRITING;
  spin_unlock_irq(&cb->lock);

  // Start writing the cache block
  bio_reset(cb->bio, ld->dev->bdev, REQ_OP_WRITE);
  cb->bio->bi_end_io  = end_flush_cache_block;
  cb->bio->bi_private = cb;
  bio_set_dev(cb->bio, ld->dev->bdev);
  cb->bio->bi_iter.bi_sector = (cb->block_number << ld->block_shift);
  cb->bio->bi_io_vec = bio_inline_vecs(cb->bio);
  cb->bio->bi_max_vecs = 1;

  bytes_added =
    bio_add_page(cb->bio, vmalloc_to_page(cb->data), ld->block_size,
                 offset_in_page(cb->data));
  if (bytes_added != ld->block_size) {
    /* This should never fail, and there's nowhere to report an error. */
    DMWARN("problem adding block data to bio");
  }
  if (ld->stopped) {
    // We are supposed to stop writing, so fail the write.
    atomic64_inc(&ld->flush_failures);
    cb->bio->bi_status = ld->error_status;
    bio_endio(cb->bio);
  } else {
    submit_bio_noacct(cb->bio);
  }

  // Grab the cache block lock, as we are expected to hold it when we return.
  spin_lock_irq(&cb->lock);
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio that can be serviced
 * using a cache block.
 *
 * @param cb     The cache block (locked)
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 **/
static void process_cached_bio(struct cache_block *cb,
                               struct bio         *bio,
                               struct bio_list    *ready)
{
  struct lossy_device *ld = cb->device;
  struct bio_vec bv;
  struct bvec_iter iter;
  sector_t block_number;
  size_t offset;
  char *data;

  // Set the block state to COPYING, and release the cache block lock.  We do
  // not want to hold the lock while we copy the data.
  cb->state = COPYING;
  spin_unlock_irq(&cb->lock);

  // Compute the cache address to begin transfers.
  block_number = bio->bi_iter.bi_sector >> ld->block_shift;
  offset = (bio->bi_iter.bi_sector - (block_number << ld->block_shift)) << SECTOR_SHIFT;
  data = cb->data + offset;

  // Copy the data.
  for (iter = bio->bi_iter; iter.bi_size > 0;
       bio_advance_iter(bio, &iter, bv.bv_len)) {
    char *buffer;

    bv = bio_iter_iovec(bio, iter);
    buffer = page_address(bv.bv_page) + bv.bv_offset;
    if (bio_data_dir(bio) == READ)
      memcpy(buffer, data, bv.bv_len);
    else
      memcpy(data, buffer, bv.bv_len);

    data += bv.bv_len;
  }

  // We are done with the bio.
  bio->bi_status = 0;
  bio_endio(bio);
  atomic64_inc(&ld->success_bios);

  // Grab the cache block lock, and set the block state to DIRTY.
  spin_lock_irq(&cb->lock);
  cb->state = DIRTY;

  // We can immediately release the waiting bios.
  bio_list_merge_init(ready, &cb->pending_bios);

  // See whether a flush request has asked to flush all blocks.  Note that this
  // check is made without holding the flush lock.  This is safe because
  // the flushing flag is true only because it was set after our bio began processing
  // and the flush_cache() missed this cache block while we were in COPYING
  // state.  The cache block spinlock has provided us with adequate memory
  // barriers.
  if (ld->flushing)
    flush_cache_block(cb);
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio that is possibly in the
 * cache.
 *
 * @param cb     The cache block (locked)
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 *
 * @return one of these specific values:
 *
 *         DM_MAPIO_REMAPPED to indicate the bio is ready for submit_bio.
 *
 *         DM_MAPIO_SUBMITTED to indicate that the bio will be processed by
 *                            this device, either by processing it completely
 *                            and calling bio_endio, or forwarding it onward
 *                            by submitting it to the next layer.
 **/
static int process_bio_locked(struct cache_block *cb,
                              struct bio         *bio,
                              struct bio_list    *ready)
{
  struct lossy_device *ld = cb->device;
  sector_t block_number = bio->bi_iter.bi_sector >> ld->block_shift;

  if (cb->state == EMPTY) {
    // Cache block is unused.  Look for a reason to do the I/O directly.  In
    // order: It's a read; it's a REQ_FUA; it's a REQ_DISCARD; it's a partial
    // block.
    if ((bio_data_dir(bio) == READ)
        || (bio->bi_opf & REQ_FUA)
        || (bio_op(bio) == REQ_OP_DISCARD)
        || (bio->bi_iter.bi_size < ld->block_size))
      return DM_MAPIO_REMAPPED;

    // We have an unused cache block for an ordinary write of a full block.
    // But filter out some blocks.  The default mask/modulus settings will
    // cause the block to be cached.  We expect to use these defaults for 4K
    // blocks.  When the blocksize if 512, we expect that the mask/modules
    // settings will be used to test with torn writes.
    if ((ld->block_mask & (1 << (block_number % ld->mask_length))) == 0)
      return DM_MAPIO_REMAPPED;

    // Use this cache block.  This is an EMPTY to DIRTY transition, so bump the
    // busy count.
    atomic_inc(&ld->busy_count);
    cb->block_number = block_number;
    process_cached_bio(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (cb->block_number != block_number) {
    // This is not the block we are looking for.
    return DM_MAPIO_REMAPPED;
  };

  // We found this block in the cache.
  if (cb->state != DIRTY) {
    // The block is busy, so we must wait.
    bio_list_add(&cb->pending_bios, bio);
    return DM_MAPIO_SUBMITTED;
  } else if (!(bio->bi_opf & REQ_FUA) && (bio_op(bio) != REQ_OP_DISCARD)) {
    // Unless it is a FUA write or a discard, we can service the bio directly
    // using the cache.
    process_cached_bio(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (bio->bi_iter.bi_size == ld->block_size) {
    // It's a full block FUA write or discard, so drop the cache block and just
    // do the write.  Because our bio is known to be busy, this can never drop
    // the busy count to zero.
    cb->state = EMPTY;
    atomic_dec(&ld->busy_count);
    return DM_MAPIO_REMAPPED;
  } else {
    // It's a partial block FUA write or discard, so wait while we flush the
    // whole cached block to storage.
    bio_list_add(&cb->pending_bios, bio);
    flush_cache_block(cb);
    return DM_MAPIO_SUBMITTED;
  }
}

/**********************************************************************/
/**
 * Flush all of the cached data to the storage medium.
 *
 * @param ld  The lossy device
 **/
static void flush_cache(struct lossy_device *ld)
{
  u16 i;

  for (i = 0; i < ld->cache_block_count; i++) {
    struct cache_block *cb = &ld->cache_blocks[i];

    spin_lock_irq(&cb->lock);
    if (cb->state == DIRTY)
      flush_cache_block(cb);
    spin_unlock_irq(&cb->lock);
  }
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio.
 *
 * @param ld     The lossy device
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 *
 * @return one of these specific values:
 *
 *         DM_MAPIO_REMAPPED to indicate that the bio is ready for submit_bio.
 *
 *         DM_MAPIO_SUBMITTED to indicate that the bio will be processed by
 *                            this device, either by processing it completely
 *                            and calling bio_endio, or forwarding it onward
 *                            by submitting it to the next layer.
 **/
static int process_bio(struct lossy_device *ld, struct bio *bio,
                       struct bio_list *ready)
{
  int result;

  if ((bio_data_dir(bio) == WRITE) && ld->stopped) {
    // We have been told to stop writing.  Make it so.
    atomic64_inc(&ld->write_failures);
    bio->bi_status = ld->error_status;
    bio_endio(bio);
    return DM_MAPIO_SUBMITTED;
  }

  if (ld->cache_block_count == 0)
    // We are not doing caching.  Just go ahead and do the I/O.
    return DM_MAPIO_REMAPPED;

  // We are doing caching.  When this busy count returns to zero, it will be
  // time to acknowledge empty flushes.
  atomic_inc(&ld->busy_count);

  spin_lock_irq(&ld->flush_lock);
  if ((bio_op(bio) == REQ_OP_FLUSH) || (bio->bi_opf & REQ_PREFLUSH)) {
    bool flushing;

    if (bio->bi_iter.bi_size > 0)
      DMWARN("flush bio too big!");

    // Add to the list of active flush bios.  If we are the first one, we must
    // initiate flushing the cache.
    bio_list_add(&ld->flush_bios, bio);
    flushing = ld->flushing;
    ld->flushing = true;
    spin_unlock_irq(&ld->flush_lock);
    if (!flushing)
      flush_cache(ld);
    result = DM_MAPIO_SUBMITTED;
  } else if (ld->flushing) {
    // A flush is in progress.  Need to defer this bio.
    bio_list_add(&ld->pending_bios, bio);
    spin_unlock_irq(&ld->flush_lock);
    result = DM_MAPIO_SUBMITTED;
  } else {
    sector_t block_number;
    u16 slotNumber;
    struct cache_block *cb;

    spin_unlock_irq(&ld->flush_lock);
    // There is no flush in progress, so we may lock the cache block and
    // proceed to do the I/O.
    block_number = bio->bi_iter.bi_sector >> ld->block_shift;
    slotNumber = block_number % ld->cache_block_count;
    cb = &ld->cache_blocks[slotNumber];
    spin_lock_irq(&cb->lock);
    result = process_bio_locked(cb, bio, ready);
    spin_unlock_irq(&cb->lock);
  }

  // We have finished working on this bio.
  complete_flushes(ld);
  return result;
}

/**********************************************************************/
/**
 * Process a list of delayed I/O requests encapsulated in a struct bio_list.
 *
 * @param ld     The lossy device
 * @param ready  bio list of bios that are ready to process
 **/
static void process_ready_bios(struct lossy_device *ld, struct bio_list *ready)
{
  struct bio *bio;

  while ((bio = bio_list_pop(ready))) {
    if (process_bio(ld, bio, ready) == DM_MAPIO_REMAPPED) {
      submit_bio_noacct(bio);
      atomic64_inc(&ld->submitted_bios);
    }
  }
}

/**********************************************************************/
static void free_cache_blocks(struct lossy_device *ld)
{
  u16 i;

  // Free the cache data blocks.
  vfree(ld->cache_data);

  // Free the bios for the cache data blocks.
  for (i = 0; i < ld->cache_block_count; i++) {
    struct cache_block *cb = &ld->cache_blocks[i];

    if (cb->bio != NULL) {
      bio_uninit(cb->bio);
      kfree(cb->bio);
    }
  }
}

/**********************************************************************/
// BEGIN device methods for the lossy target type
/**********************************************************************/
static int lossy_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
  const char *device_name;
  const char *device_path;
  struct lossy_device *ld;
  char *cache_data;
  int result;
  unsigned long long block_size;
  unsigned int cache_block_count;
  unsigned int mask_length;
  unsigned long long block_mask;
  u16 i;

  if (argc != 6) {
    ti->error = "requires exactly 6 arguments";
    return -EINVAL;
  }

  device_name = argv[0];
  device_path = argv[1];
  result = kstrtoull(argv[2], 10, &block_size);
  if (result)
    return result;
  if ((block_size != 512) && (block_size != 4096)) {
    ti->error = "Invalid block size";
    return -EINVAL;
  }

  result = kstrtouint(argv[3], 10, &cache_block_count);
  if (result)
    return result;
  if (cache_block_count > MAX_CACHE_BLOCKS) {
    ti->error = "Invalid cache size";
    return -EINVAL;
  }

  result = kstrtouint(argv[5], 10, &mask_length);
  if (result)
    return result;
  if (mask_length > MAX_MASK_LENGTH) {
    ti->error = "Modulus is too large";
    return -EINVAL;
  }

  result = kstrtoull(argv[4], 10, &block_mask);
  if (result)
    return result;
  if (block_mask >> mask_length != 0) {
    ti->error = "Mask has too many bits";
    return -EINVAL;
  }

  if (block_mask == 0 && mask_length == 0) {
    block_mask = ~0;
    mask_length = 8;
  }

  ld = kzalloc(sizeof(struct lossy_device)
               + cache_block_count * sizeof(struct cache_block),
               GFP_KERNEL);
  if (ld == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }

  if (cache_block_count > 0) {
    cache_data = __vmalloc(cache_block_count * block_size, GFP_KERNEL);
    if (cache_data == NULL) {
      kfree(ld);
      ti->error = "Cannot allocate cache";
      return -ENOMEM;
    }
  }
  ld->block_shift       = block_size == 4096 ? 3 : 0;
  ld->block_size        = block_size;
  ld->cache_data        = cache_data;
  ld->cache_block_count = cache_block_count;
  ld->error_status      = BLK_STS_IOERR;
  ld->stopped           = false;
  ld->block_mask        = block_mask;
  ld->mask_length       = mask_length;
  strncpy(ld->name, device_name, LOSSY_NAME_SIZE);
  bio_list_init(&ld->flush_bios);
  bio_list_init(&ld->pending_bios);
  bio_list_init(&ld->work_bios);
  bio_list_init(&ld->work_flush_bios);
  spin_lock_init(&ld->flush_lock);
  spin_lock_init(&ld->work_lock);
  for (i = 0; i < cache_block_count; i++) {
    struct cache_block *cb = &ld->cache_blocks[i];

    bio_list_init(&cb->pending_bios);
    spin_lock_init(&cb->lock);
    cb->bio = bio_kmalloc(1, GFP_KERNEL);
    cb->data = cache_data;
    cb->device = ld;
    cb->state = EMPTY;
    cache_data += block_size;
    if (cb->bio == NULL) {
      free_cache_blocks(ld);
      kfree(ld);
      ti->error = "Cannot allocate cache bio";
      return -ENOMEM;
    }
  }

  if (dm_get_device(ti, device_path, dm_table_get_mode(ti->table), &ld->dev)) {
    ti->error = "Device lookup failed";
    free_cache_blocks(ld);
    kfree(ld);
    return -EINVAL;
  }

  ti->flush_supported = 1;
  if (dm_set_target_max_io_len(ti, block_size >> SECTOR_SHIFT) != 0) {
    ti->error = "Set max io failed";
    dm_put_device(ti, ld->dev);
    free_cache_blocks(ld);
    kfree(ld);
    return -EINVAL;
  }

  ti->num_flush_bios = 1;
  ti->private = ld;
  return 0;
}

/**********************************************************************/

static void lossy_dtr(struct dm_target *ti)
{
  struct lossy_device *ld = ti->private;

  dm_put_device(ti, ld->dev);
  free_cache_blocks(ld);
}

/**********************************************************************/
static int lossy_prepare_ioctl(struct dm_target     *ti,
                               struct block_device **bdev,
                               unsigned int          cmd,
                               unsigned long         arg,
                               bool                 *forward)
{
  struct dm_dev *dev = ((struct lossy_device *) ti->private)->dev;

  *bdev = dev->bdev;

  // Only pass ioctls through if the device sizes match exactly.
  if (ti->len != bdev_nr_bytes(dev->bdev) >> SECTOR_SHIFT)
    return 1;

  return 0;
}

/**********************************************************************/
static int lossy_iterate_devices(struct dm_target           *ti,
                                 iterate_devices_callout_fn  fn,
                                 void                       *data)
{
  struct dm_dev *dev = ((struct lossy_device *) ti->private)->dev;

  return fn(ti, dev, 0, ti->len, data);
}

/**********************************************************************/
static int lossy_message(struct dm_target *ti, unsigned int argc, char **argv,
                         char *result, unsigned int maxlen)
{
	struct lossy_device *ld = ti->private;
	unsigned int flush_flush_count;
	unsigned int flush_bio_count;
	unsigned int work_flush_count;
	unsigned int work_bio_count;
	unsigned int sz = 0;

	if (!strcasecmp(argv[0], "stop")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMINFO("stopping");
		ld->stopped = true;
		ld->reads_at_stop = atomic64_read(&ld->total_reads);
		ld->writes_at_stop = atomic64_read(&ld->total_writes);
	} else if (!strcasecmp(argv[0], "show_mode")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMEMIT(ld->stopped ? "stop\n" : "running\n");
	} else if (!strcasecmp(argv[0], "state")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		spin_lock_irq(&ld->flush_lock);
		flush_flush_count = bio_list_size(&ld->flush_bios);
		flush_bio_count = bio_list_size(&ld->pending_bios);
		spin_unlock_irq(&ld->flush_lock);
		spin_lock_irq(&ld->work_lock);
		work_flush_count = bio_list_size(&ld->work_flush_bios);
		work_bio_count = bio_list_size(&ld->work_bios);
		spin_unlock_irq(&ld->work_lock);
		DMEMIT("block_size: %zu\n"
		       "cache_block_count: %u\n"
		       "block_mask: %u\n"
		       "mask_length: %u\n"
		       "busy_count: %d\n"
		       "stopped: %u\n"
		       "flushing: %u\n"
		       "flush_flush_count: %u\n"
		       "flush_bio_count: %u\n"
		       "work_flush_count: %u\n"
		       "work_bio_count: %u\n",
		       ld->block_size,
		       ld->cache_block_count,
		       ld->block_mask,
		       ld->mask_length,
		       atomic_read(&ld->busy_count),
		       ld->stopped,
		       ld->flushing,
		       flush_flush_count,
		       flush_bio_count,
		       work_flush_count,
		       work_bio_count);
	} else if (!strcasecmp(argv[0], "statistics")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMEMIT("reads: %lld\n"
		       "writes: %lld\n"
		       "flushes: %lld\n"
		       "FUAs: %lld\n"
		       "write_failure: %lld\n"
		       "flush_failure: %lld\n"
		       "reads_at_last_flush: %lu\n"
		       "writes_at_last_flush: %lu\n"
		       "reads_at_stop: %lu\n"
		       "writes_at_stop: %lu\n"
		       "mapped_returns: %lld\n"
		       "submitted_returns: %lld\n"
		       "submitted_bios: %lld\n"
		       "success_bios: %lld\n"
		       "error_bios: %lld\n",
		       (long long)atomic64_read(&ld->total_reads),
		       (long long)atomic64_read(&ld->total_writes),
		       (long long)atomic64_read(&ld->total_flushes),
		       (long long)atomic64_read(&ld->total_fuas),
		       (long long)atomic64_read(&ld->write_failures),
		       (long long)atomic64_read(&ld->flush_failures),
		       ld->reads_at_last_flush, ld->writes_at_last_flush,
		       ld->reads_at_stop, ld->writes_at_stop,
		       (long long)atomic64_read(&ld->mapped_returns),
		       (long long)atomic64_read(&ld->submitted_returns),
		       (long long)atomic64_read(&ld->submitted_bios),
		       (long long)atomic64_read(&ld->success_bios),
		       (long long)atomic64_read(&ld->error_bios));
	} else if (!strcasecmp(argv[0], "show_cache")) {
		u16 i;

		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		for (i = 0; i < ld->cache_block_count; i++) {
			struct cache_block *cb = &ld->cache_blocks[i];
			unsigned int waiter_count;
			sector_t sector;
			enum block_state block_state;
			char *state;

			spin_lock_irq(&cb->lock);
			waiter_count = bio_list_size(&cb->pending_bios);
			sector = cb->block_number << ld->block_shift;
			block_state = cb->state;
			spin_unlock_irq(&cb->lock);
			state = "UNKNOWN";
			switch (block_state) {
			case EMPTY:
				continue;
			case COPYING:
				state = "COPYING";
				break;
			case DIRTY:
				state = "DIRTY";
				break;
			case WRITING:
				state = "WRITING";
				break;
			default:
				break;
			}

			DMEMIT("%u %s %u %llu\n", i, state,
			       waiter_count, (unsigned long long)sector);
		}
	}

	return 0;
}

/**********************************************************************/
static int lossy_map(struct dm_target *ti,
                     struct bio       *bio)
{
  struct lossy_device *ld = ti->private;
  struct bio_list ready_bios;
  int result;

  // Map the I/O to the storage device.
  bio_set_dev(bio, ld->dev->bdev);
  bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

  // Perform accounting.
  if (bio_data_dir(bio) == READ) {
    atomic64_inc(&ld->total_reads);
  } else {
    if ((bio_op(bio) == REQ_OP_FLUSH) || (bio->bi_opf & REQ_PREFLUSH)) {
      atomic64_inc(&ld->total_flushes);
      ld->reads_at_last_flush  = atomic64_read(&ld->total_reads);
      ld->writes_at_last_flush = atomic64_read(&ld->total_writes);
    }
    if (bio->bi_opf & REQ_FUA)
      atomic64_inc(&ld->total_fuas);

    if (bio->bi_iter.bi_size > 0)
      atomic64_inc(&ld->total_writes);
  }

  // Process the already mapped I/O.
  bio_list_init(&ready_bios);
  result = process_bio(ld, bio, &ready_bios);

  // If the processing released any other bio requests, process them now.  This
  // indirect method of making a list to process one at a time ensures that we
  // do not overrun the small kernel stack.
  process_ready_bios(ld, &ready_bios);

  // Perform return value accounting.
  if (result == DM_MAPIO_REMAPPED)
    atomic64_inc(&ld->mapped_returns);
  else if (result == DM_MAPIO_SUBMITTED)
    atomic64_inc(&ld->submitted_returns);

  return result;
}

static void lossy_presuspend(struct dm_target *ti)
{
  /*
   * When suspending, pretend all writes succeeded even when stopped.
   * This can reduce complications for devices above this one.
   */
  ((struct lossy_device *) ti->private)->error_status = BLK_STS_OK;
}

static void lossy_status(struct dm_target *ti,
                         status_type_t     type,
                         unsigned int      status_flags,
                         char             *result,
                         unsigned int      maxlen)
{
  struct lossy_device *ld = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro

  switch (type) {
  case STATUSTYPE_INFO:
    result[0] = '\0';
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s %s %zu %u", ld->name, ld->dev->name, ld->block_size,
           ld->cache_block_count);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type lossy_target_type = {
  .name            = "lossy",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = lossy_ctr,
  .dtr             = lossy_dtr,
  .iterate_devices = lossy_iterate_devices,
  .map             = lossy_map,
  .message         = lossy_message,
  .presuspend      = lossy_presuspend,
  .status          = lossy_status,
  // Put version specific functions at the bottom
  .prepare_ioctl   = lossy_prepare_ioctl,
};

/**********************************************************************/
int __init lossy_init(void)
{
  int result = dm_register_target(&lossy_target_type);

  if (result < 0)
    DMERR("dm_register_target failed %d", result);
  return result;
}

/**********************************************************************/
void __exit lossy_exit(void)
{
  dm_unregister_target(&lossy_target_type);
}

module_init(lossy_init);
module_exit(lossy_exit);

MODULE_DESCRIPTION(DM_NAME " lossy testing device");
MODULE_AUTHOR("Matthew Sakai <dm-devel@lists.linux.dev>");
MODULE_LICENSE("GPL");
