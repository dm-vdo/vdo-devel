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
  struct bio_list          waitingBios;
  // Pointer back to the struct lossy_device containing this block.
  struct lossy_device     *device;
  // Pointer to the data for this block.
  char                    *blockData;
  // Pointer to the bio reserved for use when we need to write this block.
  struct bio              *blockBio;
  // The BLOCK number of this block (not the sector number).
  sector_t                 blockNumber;
  // The state of this cache block
  enum block_state         state;
};

struct lossy_device {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev  *dev;
  // Return value for unsuccessful writes.
  blk_status_t    ioError;
  // Flag that is set to true to stop all writes by the device.
  bool            stopFlag;
  // The name of the device.
  char            name[LOSSY_NAME_SIZE + 1];
  // Pointer to the cached data, used only for allocate/free of the memory.
  char           *cacheData;
  // The block size, which must be either 512 or 4K.
  size_t          blockSize;
  // Settings for producing torn writes.
  unsigned int    tornMask;
  unsigned int    tornModulus;
  // The block shift, which is used to convert sector numbers to block numbers.
  // Will be either 0 (for blockSize 512) or 3 (for blockSize 4K).
  unsigned int    blockShift;
  // The number of cache blocks, which may be zero for no block cache.
  unsigned int    cacheBlockCount;
  // The busy count of the device, which is used to implement proper REQ_FLUSH
  // requests when there is a block cache.  It counts the number of bios that
  // we are actively working on, and the number of dirty blocks in the block
  // cache.  An REQ_FLUSH request cannot be completed until this count goes to
  // zero.
  atomic_t        busyCount;

  // BEGIN data that pertains to work done in a kworker thread for this
  // device.  This spin lock protects these data, and it is taken by the
  // bi_end_io callback when we write a cache block, and therefore should be
  // used with spin_lock_irq or spin_lock_irqsave.
  spinlock_t         workLock;
  // When the processing of a bio has been delayed, it will eventually be put
  // on this list and processed in a kworker thread.
  struct bio_list    workBios;
  // When the processing of an REQ_FLUSH request has been completed, it will be
  // put on this list and processed in a kworker thread.
  struct bio_list    workFlushBios;
  // This is a Linux work item used to schedule processing of the workBios
  // list.
  struct work_struct workWork;
  // END of data protected by workLock.

  // BEGIN data that pertains to processing REQ_FLUSH requests.  This spin lock
  // protects these data, and may be taken by the bi_end_io callback when we
  // write a cache block, and therefore should be used with spin_lock_irq or
  // spin_lock_irqsave.
  spinlock_t      flushLock;
  // A flag to indicate that a flush is in progress.
  bool            flushFlag;
  // When an REQ_FLUSH bio arrives, it will be put onto this list for
  // processing at the proper time.
  struct bio_list flushBios;
  // When flushFlag is set, all non-REQ_FLUSH bios are put onto this list for
  // processing when the flush is completed.
  struct bio_list waitingBios;
  // END of data protected by flushLock.

  // BEGIN data that are merely statistics and do not effect code behavior.
  // These stats count the bios that arrive into the lossyMap method.
  atomic64_t    readTotal;
  atomic64_t    writeTotal;
  atomic64_t    flushTotal;
  atomic64_t    fuaTotal;
  atomic64_t    writeFailure;
  atomic64_t    flushFailure;
  unsigned long readsAtLastFlush;
  unsigned long writesAtLastFlush;
  unsigned long readsAtStop;
  unsigned long writesAtStop;
  // These stats count the values returned by the lossyMap method.
  atomic64_t    mappedReturns;
  atomic64_t    submittedReturns;
  // These stats count the bios for which the lossyMap method returned
  // "submitted".
  atomic64_t    submittedBios;
  atomic64_t    successBios;
  atomic64_t    errorBios;
  // END of statistics

  // The block cache (variable sized, so it goes at the end).
  struct cache_block cacheBlocks[];
};

/**********************************************************************/
// BEGIN large section of code for the block cache
/**********************************************************************/

static void processBioList(struct lossy_device *ld, struct bio_list *ready);

/**
 * Do delayed processing of a list of bios in a kworker thread.
 *
 * @param work  A kworker work struct.
 **/
static void processDelayed(struct work_struct *work)
{
  struct lossy_device *ld = container_of(work, struct lossy_device, workWork);

  // Under the worklock, grab the lists of bios to be processed.
  struct bio_list flushes, ready;
  bio_list_init(&flushes);
  bio_list_init(&ready);
  spin_lock_irq(&ld->workLock);
  bio_list_merge_init(&flushes, &ld->workFlushBios);
  bio_list_merge_init(&ready, &ld->workBios);
  spin_unlock_irq(&ld->workLock);

  // Process the completed flushes.
  struct bio *bio;
  while ((bio = bio_list_pop(&flushes))) {
    if (ld->stopFlag && (atomic64_read(&ld->writeFailure) > 0)) {
      // We are stopping writes and failed to write a cached block.
      bio->bi_status = ld->ioError;
      bio_endio(bio);
      atomic64_inc(&ld->errorBios);
    } else {
      // Still succeeding, so forward the flush to the storage medium.
      submit_bio_noacct(bio);
      atomic64_inc(&ld->submittedBios);
    }
  }

  // Process the delayed bios.
  processBioList(ld, &ready);
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
static void scheduleDelayedProcessing(struct lossy_device *ld,
                                      struct bio_list     *ready,
                                      struct bio_list     *flushes)
{
  bool haveBios = !bio_list_empty(ready);
  bool haveFlushes = flushes && !bio_list_empty(flushes);

  // If the lists of new bios are empty, there is nothing to do.
  if (!haveFlushes && !haveBios)
    return;

  // Under the worklock, add the new bios to the existing lists of bios to
  // process.
  unsigned long flags;
  spin_lock_irqsave(&ld->workLock, flags);
  bool schedulingNeeded = (bio_list_empty(&ld->workBios)
                           && bio_list_empty(&ld->workFlushBios);
  if (haveBios)
    bio_list_merge_init(&ld->workBios, ready);
  if (haveFlushes)
    bio_list_merge_init(&ld->workFlushBios, flushes);
  spin_unlock_irqrestore(ld->workLock, flags);

  // If we added to empty lists, schedule a work item.  Otherwise there is
  // already a work item scheduled.
  if (schedulingNeeded) {
    INIT_WORK(&ld->workWork, processDelayed);
    schedule_work(&ld->workWork);
  }
}

/**********************************************************************/
/**
 * Decrement the busy count.  If it goes to zero and a flush is in progress,
 * finish the flush.  This method can be called from a bi_end_io callback.
 *
 * @param ld  The lossy device
 **/
static void decrementBusyCountAndTest(struct lossy_device *ld)
{
  if (atomic_dec_and_test(&ld->busyCount)) {
    // The busy count has just dropped to zero, so we need to take flushLock
    // and deal with any flushes in progress.
    struct bio_list completedFlushes, readyBios;
    unsigned long flags;
    bio_list_init(&completedFlushes);
    bio_list_init(&readyBios);
    spin_lock_irqsave(&ld->flushLock, flags);
    if (ld->flushFlag) {
      // And there are REQ_FLUSH requests in progress.
      ld->flushFlag = false;
      // Record the flush bios that are complete.
      bio_list_merge_init(&completedFlushes, &ld->flushBios);
      // Record the bios that are now ready to start.
      bio_list_merge_init(&readyBios, &ld->waitingBios);
     }
    spin_unlock_irqrestore(&ld->flushLock, flags);

    // Start the "ready" ones.
    scheduleDelayedProcessing(ld, &readyBios, &completedFlushes);
  }
}

/**********************************************************************/
/**
 * bi_end_io callback routine for when a cache block write completes
 *
 * @param bio    The bio
 **/
static void endFlushCacheBlock(struct bio *bio)
{
  int error = blk_status_to_errno(bio->bi_status);
  struct cache_block *cb = bio->bi_private;
  struct lossy_device *ld = cb->device;
  struct bio_list ready;
  bio_list_init(&ready);

  if (error)
    DMWARN("error flushing at sector %llu: %d\n",
           (unsigned long long) (cb->blockNumber << ld->blockShift), error);

  // Set the block state to EMPTY.  This is a transition from WRITING to EMPTY.
  unsigned long flags;
  spin_lock_irqsave(&cb->lock, flags);
  cb->state = EMPTY;
  // Record the bios that are now ready to start
  bio_list_merge_init(&ready, &cb->waitingBios);
  spin_unlock_irqrestore(&cb->lock, flags);

  // Finish the transition to EMPTY.
  decrementBusyCountAndTest(ld);

  // Start any bios that were waiting for this specific cache block.
  scheduleDelayedProcessing(ld, &ready, NULL);
}

/**********************************************************************/
/**
 * Flush a cache block to storage.
 *
 * @param cb  The cache block (locked)
 **/
static void flushCacheBlock(struct cache_block *cb)
{
  struct lossy_device *ld = cb->device;

  // Set the block state to WRITING, and release the cache block lock.  We do
  // not want to hold the lock while we write the data.
  cb->state = WRITING;
  spin_unlock_irq(&cb->lock);

  // Start writing the cache block
  bio_reset(cb->blockBio, ld->dev->bdev, REQ_OP_WRITE);
  cb->blockBio->bi_end_io  = endFlushCacheBlock;
  cb->blockBio->bi_private = cb;
  bio_set_dev(cb->blockBio, ld->dev->bdev);
  cb->blockBio->bi_iter.bi_sector = (cb->blockNumber << ld->blockShift);
  cb->blockBio->bi_io_vec = bio_inline_vecs(cb->blockBio);
  cb->blockBio->bi_max_vecs = 1;

  int bytes_added =
    bio_add_page(cb->blockBio, vmalloc_to_page(cb->blockData), ld->blockSize,
                 offset_in_page(cb->blockData));
  if (bytes_added != ld->blockSize) {
    /* This should never fail, and there's nowhere to report an error. */
    DMWARN("problem adding block data to bio");
  }
  if (ld->stopFlag) {
    // We are supposed to stop writing, so fail the write.
    atomic64_inc(&ld->flushFailure);
    cb->blockBio->bi_status = ld->ioError;
    bio_endio(cb->blockBio);
  } else {
    submit_bio_noacct(cb->blockBio);
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
static void processBioCached(struct cache_block *cb,
                             struct bio         *bio,
                             struct bio_list    *ready)
{
  struct lossy_device *ld = cb->device;

  // Set the block state to COPYING, and release the cache block lock.  We do
  // not want to hold the lock while we copy the data.
  cb->state = COPYING;
  spin_unlock_irq(&cb->lock);

  // Compute the cache address to begin transfers.
  sector_t blockNumber = bio->bi_iter.bi_sector >> ld->blockShift;
  size_t offset = (bio->bi_iter.bi_sector - (blockNumber << ld->blockShift)) << SECTOR_SHIFT;
  char *data = cb->blockData + offset;

  // Copy the data.
  struct bio_vec bv;
  for (struct bvec_iter iter = bio->bi_iter; iter.bi_size > 0;
       bio_advance_iter(bio, &iter, bv.bv_len)) {
    bv = bio_iter_iovec(bio, iter);
    char *buffer = page_address(bv.bv_page) + bv.bv_offset;
    if (bio_data_dir(bio) == READ)
      memcpy(buffer, data, bv.bv_len);
    else
      memcpy(data, buffer, bv.bv_len);

    data += bv.bv_len;
  }

  // We are done with the bio.
  bio->bi_status = 0;
  bio_endio(bio);
  atomic64_inc(&ld->successBios);

  // Grab the cache block lock, and set the block state to DIRTY.
  spin_lock_irq(&cb->lock);
  cb->state = DIRTY;

  // We can immediately release the waiting bios.
  bio_list_merge_init(ready, &cb->waitingBios);

  // See whether a flush request has asked to flush all blocks.  Note that this
  // check is made without holding the flush lock.  This is safe because
  // flushFlag is true only because it was set after our bio began processing
  // and the flushTheCache() missed this cache block while we were in COPYING
  // state.  The cache block spinlock has provided us with adequate memory
  // barriers.
  if (ld->flushFlag)
    flushCacheBlock(cb);
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
static int processBioLocked(struct cache_block *cb,
                            struct bio         *bio,
                            struct bio_list    *ready)
{
  struct lossy_device *ld = cb->device;
  sector_t blockNumber = bio->bi_iter.bi_sector >> ld->blockShift;
  if (cb->state == EMPTY) {
    // Cache block is unused.  Look for a reason to do the I/O directly.  In
    // order: It's a read; it's a REQ_FUA; it's a REQ_DISCARD; it's a partial
    // block.
    if ((bio_data_dir(bio) == READ)
        || (bio->bi_opf & REQ_FUA)
        || (bio_op(bio) == REQ_OP_DISCARD)
        || (bio->bi_iter.bi_size < ld->blockSize))
      return DM_MAPIO_REMAPPED;

    // We have an unused cache block for an ordinary write of a full block.
    // But filter out some blocks.  The default mask/modulus settings will
    // cause the block to be cached.  We expect to use these defaults for 4K
    // blocks.  When the blocksize if 512, we expect that the mask/modules
    // settings will be used to test with torn writes.
    if ((ld->tornMask & (1 << (blockNumber % ld->tornModulus))) == 0)
      return DM_MAPIO_REMAPPED;

    // Use this cache block.  This is an EMPTY to DIRTY transition, so bump the
    // busy count.
    atomic_inc(&ld->busyCount);
    cb->blockNumber = blockNumber;
    processBioCached(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (cb->blockNumber != blockNumber) {
    // This is not the block we are looking for.
    return DM_MAPIO_REMAPPED;
  };

  // We found this block in the cache.
  if (cb->state != DIRTY) {
    // The block is busy, so we must wait.
    bio_list_add(&cb->waitingBios, bio);
    return DM_MAPIO_SUBMITTED;
  } else if (!(bio->bi_opf & REQ_FUA) && (bio_op(bio) != REQ_OP_DISCARD)) {
    // Unless it is a FUA write or a discard, we can service the bio directly
    // using the cache.
    processBioCached(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (bio->bi_iter.bi_size == ld->blockSize) {
    // It's a full block FUA write or discard, so drop the cache block and just
    // do the write.  Because our bio is known to be busy, this can never drop
    // the busy count to zero.
    cb->state = EMPTY;
    atomic_dec(&ld->busyCount);
    return DM_MAPIO_REMAPPED;
  } else {
    // It's a partial block FUA write or discard, so wait while we flush the
    // whole cached block to storage.
    bio_list_add(&cb->waitingBios, bio);
    flushCacheBlock(cb);
    return DM_MAPIO_SUBMITTED;
  }
}

/**********************************************************************/
/**
 * Flush all of the cached data to the storage medium.
 *
 * @param ld  The lossy device
 **/
static void flushTheCache(struct lossy_device *ld)
{
  for (unsigned int i = 0; i < ld->cacheBlockCount; i++) {
    struct cache_block *cb = &ld->cacheBlocks[i];
    spin_lock_irq(&cb->lock);
    if (cb->state == DIRTY)
      flushCacheBlock(cb);
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
static int processBio(struct lossy_device *ld, struct bio *bio,
                      struct bio_list *ready)
{
  if ((bio_data_dir(bio) == WRITE) && ld->stopFlag) {
    // We have been told to stop writing.  Make it so.
    atomic64_inc(&ld->writeFailure);
    bio->bi_status = ld->ioError;
    bio_endio(bio);
    return DM_MAPIO_SUBMITTED;
  }

  if (ld->cacheBlockCount == 0)
    // We are not doing caching.  Just go ahead and do the I/O.
    return DM_MAPIO_REMAPPED;

  // We are doing caching.  When this busy count returns to zero, it will be
  // time to acknowledge empty flushes.
  atomic_inc(&ld->busyCount);

  spin_lock_irq(&ld->flushLock);
  int result;
  if ((bio_op(bio) == REQ_OP_FLUSH) || (bio->bi_opf & REQ_PREFLUSH)) {
    if (bio->bi_iter.bi_size > 0)
      DMWARN("flush bio too big!");

    // Add to the list of active flush bios.  If we are the first one, we must
    // initiate flushing the cache.
    bio_list_add(&ld->flushBios, bio);
    bool firstFlush = !ld->flushFlag;
    ld->flushFlag = true;
    spin_unlock_irq(&ld->flushLock);
    if (firstFlush)
      flushTheCache(ld);
    result = DM_MAPIO_SUBMITTED;
  } else if (ld->flushFlag) {
    // A flush is in progress.  Need to defer this bio.
    bio_list_add(&ld->waitingBios, bio);
    spin_unlock_irq(&ld->flushLock);
    result = DM_MAPIO_SUBMITTED;
  } else {
    spin_unlock_irq(&ld->flushLock);
    // There is no flush in progress, so we may lock the cache block and
    // proceed to do the I/O.
    sector_t blockNumber = bio->bi_iter.bi_sector >> ld->blockShift;
    unsigned int slotNumber = blockNumber % ld->cacheBlockCount;
    struct cache_block *cb = &ld->cacheBlocks[slotNumber];
    spin_lock_irq(&cb->lock);
    result = processBioLocked(cb, bio, ready);
    spin_unlock_irq(&cb->lock);
  }

  // We have finished working on this bio.
  decrementBusyCountAndTest(ld);
  return result;
}

/**********************************************************************/
/**
 * Process a list of delayed I/O requests encapsulated in a struct bio_list.
 *
 * @param ld     The lossy device
 * @param ready  bio list of bios that are ready to process
 **/
static void processBioList(struct lossy_device *ld, struct bio_list *ready)
{
  struct bio *bio;
  while ((bio = bio_list_pop(ready))) {
    if (processBio(ld, bio, ready) == DM_MAPIO_REMAPPED) {
      submit_bio_noacct(bio);
      atomic64_inc(&ld->submittedBios);
    }
  }
}

/**********************************************************************/
static void freeLossyDeviceCache(struct lossy_device *ld)
{
  // Free the cache data blocks.
  vfree(ld->cacheData);

  // Free the bios for the cache data blocks.
  for (unsigned int i = 0; i < ld->cacheBlockCount; i++) {
    struct cache_block *cb = &ld->cacheBlocks[i];
    if (cb->blockBio != NULL) {
      bio_uninit(cb->blockBio);
      kfree(cb->blockBio);
    }
  }
}

/**********************************************************************/
// BEGIN device methods for the lossy target type
/**********************************************************************/
static int lossyCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 4) {
    ti->error = "requires exactly 4 arguments";
    return -EINVAL;
  }
  const char *deviceName = argv[0];
  const char *devicePath = argv[1];
  int result;
  unsigned long long blockSize, cacheBlockCount;

  result = kstrtoull(argv[2], 10, &blockSize);
  if (result)
    return result;
  if ((blockSize != 512) && (blockSize != 4096)) {
    ti->error = "Invalid block size";
    return -EINVAL;
  }

  result = kstrtoull(argv[3], 10, &cacheBlockCount);
  if (result)
    return result;
  if (cacheBlockCount > MAX_CACHE_BLOCKS) {
    ti->error = "Invalid cache size";
    return -EINVAL;
  }

  struct lossy_device *ld
    = kzalloc(sizeof(struct lossy_device)
              + cacheBlockCount * sizeof(struct cache_block),
              GFP_KERNEL);
  if (ld == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }
  char *cacheData = NULL;
  if (cacheBlockCount > 0) {
    cacheData = __vmalloc(cacheBlockCount * blockSize, GFP_KERNEL);
    if (cacheData == NULL) {
      kfree(ld);
      ti->error = "Cannot allocate cache";
      return -ENOMEM;
    }
  }
  ld->blockShift      = blockSize == 4096 ? 3 : 0;
  ld->blockSize       = blockSize;
  ld->cacheData       = cacheData;
  ld->cacheBlockCount = cacheBlockCount;
  ld->ioError         = BLK_STS_IOERR;
  ld->stopFlag        = false;
  ld->tornMask        = ~0;
  ld->tornModulus     = 8;
  strncpy(ld->name, deviceName, LOSSY_NAME_SIZE);
  bio_list_init(&ld->flushBios);
  bio_list_init(&ld->waitingBios);
  bio_list_init(&ld->workBios);
  bio_list_init(&ld->workFlushBios);
  spin_lock_init(&ld->flushLock);
  spin_lock_init(&ld->workLock);
  for (unsigned int i = 0; i < cacheBlockCount; i++) {
    struct cache_block *cb = &ld->cacheBlocks[i];
    bio_list_init(&cb->waitingBios);
    spin_lock_init(&cb->lock);
    cb->blockBio = bio_kmalloc(1, GFP_KERNEL);
    cb->blockData = cacheData;
    cb->device = ld;
    cb->state = EMPTY;
    cacheData += blockSize;
    if (cb->blockBio == NULL) {
      freeLossyDeviceCache(ld);
      kfree(ld);
      ti->error = "Cannot allocate cache bio";
      return -ENOMEM;
    }
  }

  if (dm_get_device(ti, devicePath, dm_table_get_mode(ti->table), &ld->dev)) {
    ti->error = "Device lookup failed";
    freeLossyDeviceCache(ld);
    kfree(ld);
    return -EINVAL;
  }

  ti->flush_supported = 1;
  if (dm_set_target_max_io_len(ti, blockSize >> SECTOR_SHIFT) != 0) {
    ti->error = "Set max io failed";
    dm_put_device(ti, ld->dev);
    freeLossyDeviceCache(ld);
    kfree(ld);
    return -EINVAL;
  }

  ti->num_flush_bios = 1;
  ti->private = ld;
  return 0;
}

/**********************************************************************/
static void lossyDtr(struct dm_target *ti)
{
  struct lossy_device *ld = ti->private;
  dm_put_device(ti, ld->dev);
  freLossyDeviceCache(ld);
}

/**********************************************************************/
static int lossyPrepareIoctl(struct dm_target     *ti,
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
static int lossyIterateDevices(struct dm_target           *ti,
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
	int errval;
	unsigned int val;

	if (!strcasecmp(argv[0], "stop")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMINFO("stopping");
		ld->stopFlag = true;
		ld->readsAtStop = atomic64_read(&ld->readTotal);
		ld->writesAtStop = atomic64_read(&ld->writeTotal);
	} else if (!strcasecmp(argv[0], "return_eio")) {
		if (argc != 2) {
			DMERR("%s takes exactly one argument", argv[0]);
			return -EINVAL;
		}

		errval = kstrtouint(argv[1], 0, &val);
		if (errval)
			return errval;

		if (val == 0)
			ld->ioError = BLK_STS_OK;
		else if (val == 1)
			ld->ioError = BLK_STS_IOERR;
	} else if (!strcasecmp(argv[0], "torn_mask")) {
		if (argc != 2) {
			DMERR("%s takes exactly one argument", argv[0]);
			return -EINVAL;
		}

		errval = kstrtouint(argv[1], 0, &val);
		if (errval)
			return errval;

		ld->tornMask = val;
	} else if (!strcasecmp(argv[0], "torn_modulus")) {
		if (argc != 2) {
			DMERR("%s takes exactly one argument", argv[0]);
			return -EINVAL;
		}

		errval = kstrtouint(argv[1], 0, &val);
		if (errval)
			return errval;

		ld->tornModulus = val;
	} else if (!strcasecmp(argv[0], "show_mode")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMEMIT(ld->stopFlag ? "stop\n" : "running\n");
	} else if (!strcasecmp(argv[0], "show_modulus")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMEMIT("%u\n", ld->tornModulus);
	} else if (!strcasecmp(argv[0], "show_mask")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		DMEMIT("%u\n", ld->tornMask);
	} else if (!strcasecmp(argv[0], "state")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		spin_lock_irq(&ld->flushLock);
		flush_flush_count = bio_list_size(&ld->flushBios);
		flush_bio_count = bio_list_size(&ld->waitingBios);
		spin_unlock_irq(&ld->flushLock);
		spin_lock_irq(&ld->workLock);
		work_flush_count = bio_list_size(&ld->workFlushBios);
		work_bio_count = bio_list_size(&ld->workBios);
		spin_unlock_irq(&ld->workLock);
		DMEMIT("block_size: %zu\n"
		       "cache_block_count: %u\n"
		       "torn_mask: %u\n"
		       "torn_modulus: %u\n"
		       "busy_count: %d\n"
		       "stop_flag: %u\n"
		       "flush_flag: %u\n"
		       "flush_flush_count: %u\n"
		       "flush_bio_count: %u\n"
		       "work_flush_count: %u\n"
		       "work_bio_count: %u\n",
		       ld->blockSize,
		       ld->cacheBlockCount,
		       ld->tornMask,
		       ld->tornModulus,
		       atomic_read(&ld->busyCount),
		       ld->stopFlag,
		       ld->flushFlag,
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
		       (long long)atomic64_read(&ld->readTotal),
		       (long long)atomic64_read(&ld->writeTotal),
		       (long long)atomic64_read(&ld->flushTotal),
		       (long long)atomic64_read(&ld->fuaTotal),
		       (long long)atomic64_read(&ld->writeFailure),
		       (long long)atomic64_read(&ld->flushFailure),
		       ld->readsAtLastFlush, ld->writesAtLastFlush,
		       ld->readsAtStop, ld->writesAtStop,
		       (long long)atomic64_read(&ld->mappedReturns),
		       (long long)atomic64_read(&ld->submittedReturns),
		       (long long)atomic64_read(&ld->submittedBios),
		       (long long)atomic64_read(&ld->successBios),
		       (long long)atomic64_read(&ld->errorBios));
	} else if (!strcasecmp(argv[0], "show_cache")) {
		if (argc != 1) {
			DMERR("%s takes no arguments", argv[0]);
			return -EINVAL;
		}

		for (unsigned int i = 0; i < ld->cacheBlockCount; i++) {
			struct cache_block *cb = &ld->cacheBlocks[i];
			unsigned int waiter_count;
			sector_t sector;
			enum block_state block_state;
			char *state;

			spin_lock_irq(&cb->lock);
			waiter_count = bio_list_size(&cb->waitingBios);
			sector = cb->blockNumber << ld->blockShift;
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
static int lossyMap(struct dm_target *ti,
                    struct bio       *bio)
{
  struct lossy_device *ld = ti->private;

  // Map the I/O to the storage device.
  bio_set_dev(bio, ld->dev->bdev);
  bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

  // Perform accounting.
  if (bio_data_dir(bio) == READ) {
    atomic64_inc(&ld->readTotal);
  } else {
    if ((bio_op(bio) == REQ_OP_FLUSH) || (bio->bi_opf & REQ_PREFLUSH)) {
      atomic64_inc(&ld->flushTotal);
      ld->readsAtLastFlush  = atomic64_read(&ld->readTotal);
      ld->writesAtLastFlush = atomic64_read(&ld->writeTotal);
    }
    if (bio->bi_opf & REQ_FUA)
      atomic64_inc(&ld->fuaTotal);

    if (bio->bi_iter.bi_size > 0)
      atomic64_inc(&ld->writeTotal);
  }

  // Process the already mapped I/O.
  struct bio_list readyList;
  bio_list_init(&readyList);
  int result = processBio(ld, bio, &readyList);

  // If the processing released any other bio requests, process them now.  This
  // indirect method of making a list to process one at a time ensures that we
  // do not overrun the small kernel stack.
  processBioList(ld, &readyList);

  // Perform return value accounting.
  if (result == DM_MAPIO_REMAPPED)
    atomic64_inc(&ld->mappedReturns);
  else if (result == DM_MAPIO_SUBMITTED)
    atomic64_inc(&ld->submittedReturns);

  return result;
}

/**********************************************************************/
static void lossyStatus(struct dm_target *ti,
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
    DMEMIT("%s %s %zu %u", ld->name, ld->dev->name, ld->blockSize,
           ld->cacheBlockCount);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type lossyTargetType = {
  .name            = "lossy",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = lossyCtr,
  .dtr             = lossyDtr,
  .iterate_devices = lossyIterateDevices,
  .map             = lossyMap,
  .message         = lossy_message,
  .status          = lossyStatus,
  // Put version specific functions at the bottom
  .prepare_ioctl   = lossyPrepareIoctl,
};

/**********************************************************************/
int __init lossyInit(void)
{
  int result = dm_register_target(&lossyTargetType);
  if (result < 0)
    DMERR("dm_register_target failed %d", result);
  return result;
}

/**********************************************************************/
void __exit lossyExit(void)
{
  dm_unregister_target(&lossyTargetType);
}

module_init(lossyInit);
module_exit(lossyExit);

MODULE_DESCRIPTION(DM_NAME " lossy testing device");
MODULE_AUTHOR("Matthew Sakai <dm-devel@lists.linux.dev>");
MODULE_LICENSE("GPL");
