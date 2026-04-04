// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This is a test target designed to create specific data patterns on the
 * underlying block device by selectively preventing writes. It is intended to
 * help tests create data patterns that are technically possible, but difficult
 * to create through simpler means. It uses adversarial flushing, intentionally
 * holding back unflushed writes, to test the limits of the flush guarantees of
 * other storage targets.
 *
 * In normal mode, dm-lossy acts as a passthrough to the device under it. At
 * any time, a message can be used to stop dm-lossy and prevent any future
 * writes, as well as "forgetting" any previously acknowledged writes which
 * were not flushed. In addition, once dm-lossy is suspended in either mode,
 * it will no longer return errors for rejected writes, allowing any targets
 * above it to be suspended and removed without any extra error handling.
 */

#include <linux/blk_types.h>
#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <uapi/linux/dm-ioctl.h>
#ifndef VDO_UPSTREAM

/* only needed for tests; see dm-vdo-target.c */
#include "dm-lossy-target.h"
#endif /* VDO_UPSTREAM */

#define MAX_CACHE_BLOCKS 0xFFEC
#define MAX_MASK_LENGTH 64
#define LOSSY_NAME_SIZE 12
#define DM_MSG_PREFIX "lossy"

enum __attribute__((__packed__)) block_state {
  /* This cache block has no data yet */
  EMPTY,
  /* This cache block has data and is not changing */
  DIRTY,
  /*
   * Some I/O is currently using the cache block contents. This can
   * be an incoming write storing data in this cache block, or else
   * the block contents are being written to the underlying storage.
   * Incoming I/O must wait for the current operation to completee.
   */
  WRITING,
};

struct lossy_device;

struct cache_block {
  /* A spin lock that protects the cache block state and bio lists */
  spinlock_t               lock;
  /* A list of bios waiting for current I/O to complete */
  struct bio_list          pending_bios;
  /* A pointer to the struct lossy_device containing this block */
  struct lossy_device     *device;
  /* A pointer to the data for this block */
  char                    *data;
  /* A pointer to the bio reserved for writing this block */
  struct bio              *bio;
  /* The block number of the data in this cache block */
  sector_t                 block_number;
  /* The state of this cache block */
  enum block_state         state;
};

struct lossy_device {
  /* A pointer to the underlying storage device */
  struct dm_dev  *dev;
  /* Return value for failed or forgotten writes */
  blk_status_t    error_status;
  /* Flag indicating that this device will no longer process writes */
  bool            stopped;
  /* The name of the device */
  char            name[LOSSY_NAME_SIZE + 1];
  /* A pointer to the data array for cache blocks */
  char           *cache_data;
  /* The block size (must be 512 or 4096) */
  size_t          block_size;
  /* Mask indicating which block addresses will be cached */
  u32             block_mask;
  /* The number of bits in the block mask */
  u32             mask_length;
  /* The bit shift used to convert block numbers to sector numbers */
  u16             block_shift;
  /* The number of cache blocks */
  u16             cache_block_count;
  /* The number of unflushed block addresses currently in use */
  atomic_t        busy_count;

  /* A spin lock protecting the kernel work item and its bio lists */
  spinlock_t         work_lock;
  /* Regular data bios to process from the work item */
  struct bio_list    work_bios;
  /* Flush bios to process from the work item */
  struct bio_list    work_flush_bios;
  /* The work item for deferred bio processing; we only need one */
  struct work_struct work_item;

  /* A spin lock for managing flush progress */
  spinlock_t      flush_lock;
  /* A flag indicating that a flush is currently in progress */
  bool            flushing;
  /* A list of flush bios not yet completed */
  struct bio_list flush_bios;
  /* A list of data bios that must wait for a flush to complete */
  struct bio_list pending_bios;

  /* Counters used to track statistics */
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
  atomic64_t    mapped_returns;
  atomic64_t    submitted_returns;
  atomic64_t    submitted_bios;
  atomic64_t    success_bios;
  atomic64_t    error_bios;

  /* The array of cache blocks */
  struct cache_block cache_blocks[];
};

static void process_ready_bios(struct lossy_device *ld, struct bio_list *ready);

/**
 * process_pending_bios() - Do delayed processing of a list of bios in a kworker thread.
 * @work: A kworker work struct.
 */
static void process_pending_bios(struct work_struct *work)
{
  struct lossy_device *ld = container_of(work, struct lossy_device, work_item);
  struct bio_list flushes;
  struct bio_list ready;
  struct bio *bio;

  bio_list_init(&flushes);
  bio_list_init(&ready);
  spin_lock_irq(&ld->work_lock);
  bio_list_merge_init(&flushes, &ld->work_flush_bios);
  bio_list_merge_init(&ready, &ld->work_bios);
  spin_unlock_irq(&ld->work_lock);

  while ((bio = bio_list_pop(&flushes))) {
    if (ld->stopped && (atomic64_read(&ld->write_failures) > 0)) {
      /* If any writes failed, the flush should fail, too. */
      bio->bi_status = ld->error_status;
      bio_endio(bio);
      atomic64_inc(&ld->error_bios);
    } else {
      submit_bio_noacct(bio);
      atomic64_inc(&ld->submitted_bios);
    }
  }

  process_ready_bios(ld, &ready);
}

/**
 * schedule_pending_bios() - Schedule delayed processing of bios.
 * @param ld: The lossy device
 * @param ready: bio list of bios that are now unblocked
 * @param flushes: bio list of REQ_FLUSH bios that are now complete
 */
static void schedule_pending_bios(struct lossy_device *ld,
                                  struct bio_list     *ready,
                                  struct bio_list     *flushes)
{
  bool new_bios = !bio_list_empty(ready);
  bool new_flushes = flushes && !bio_list_empty(flushes);
  bool schedule_work_item;
  unsigned long flags;

  if (!new_flushes && !new_bios)
    return;

  spin_lock_irqsave(&ld->work_lock, flags);
  schedule_work_item = (bio_list_empty(&ld->work_bios)
                        && bio_list_empty(&ld->work_flush_bios));
  if (new_bios)
    bio_list_merge_init(&ld->work_bios, ready);
  if (new_flushes)
    bio_list_merge_init(&ld->work_flush_bios, flushes);
  spin_unlock_irqrestore(&ld->work_lock, flags);

  /* Schedule the work item if it is not already in flight */
  if (schedule_work_item) {
    INIT_WORK(&ld->work_item, process_pending_bios);
    schedule_work(&ld->work_item);
  }
}

/**
 * complete_flushes() - Complete bio processing and handle completed flushes.
 * @ld: The lossy device
 *
 * This function can be called from a bi_end_io callback.
 */
static void complete_flushes(struct lossy_device *ld)
{
  struct bio_list completed_flushes;
  struct bio_list ready_bios;
  unsigned long flags;

  if (atomic_dec_and_test(&ld->busy_count)) {
    /* Handle any flushes waiting on the busy count */
    bio_list_init(&completed_flushes);
    bio_list_init(&ready_bios);
    spin_lock_irqsave(&ld->flush_lock, flags);
    if (ld->flushing) {
      ld->flushing = false;
      bio_list_merge_init(&completed_flushes, &ld->flush_bios);
      bio_list_merge_init(&ready_bios, &ld->pending_bios);
     }
    spin_unlock_irqrestore(&ld->flush_lock, flags);
    schedule_pending_bios(ld, &ready_bios, &completed_flushes);
  }
}

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

  spin_lock_irqsave(&cb->lock, flags);
  cb->state = EMPTY;
  bio_list_merge_init(&ready, &cb->pending_bios);
  spin_unlock_irqrestore(&cb->lock, flags);
  complete_flushes(ld);

  schedule_pending_bios(ld, &ready, NULL);
}

/**
 * flush_cache_block() - Flush a cache block to storage.
 * @cb: The cache block (locked)
 */
static void flush_cache_block(struct cache_block *cb)
{
  struct lossy_device *ld = cb->device;
  int bytes_added;

  cb->state = WRITING;
  spin_unlock_irq(&cb->lock);

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
    atomic64_inc(&ld->flush_failures);
    cb->bio->bi_status = ld->error_status;
    bio_endio(cb->bio);
  } else {
    submit_bio_noacct(cb->bio);
  }

  /* The caller expects to still hold the cache block lock */
  spin_lock_irq(&cb->lock);
}

/**
 * process_cached_bio() - Handle an I/O request from a cached block.
 * @cb: The cache block (locked)
 * @bio: The bio to be processed
 * @ready: The list of unblocked bios that should be handled after returning
 */
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

  /* Copy the new data into the cache block */
  cb->state = WRITING;
  spin_unlock_irq(&cb->lock);

  block_number = bio->bi_iter.bi_sector >> ld->block_shift;
  offset = (bio->bi_iter.bi_sector - (block_number << ld->block_shift)) << SECTOR_SHIFT;
  data = cb->data + offset;

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

  bio->bi_status = 0;
  bio_endio(bio);
  atomic64_inc(&ld->success_bios);

  spin_lock_irq(&cb->lock);
  cb->state = DIRTY;
  bio_list_merge_init(ready, &cb->pending_bios);

  /*
   * If a flush is happening, it may have missed this block because it
   * wasn't marked DIRTY yet. Flush the block now.
   */
  if (ld->flushing)
    flush_cache_block(cb);
}

/**
 * process_bio_locked() - Handle bio processing while holding cache block lock.
 * @cb: The cache block (locked)
 * @bio: The bio to be processed
 * @ready: The list of unblocked bios that should be handled after returning
 *
 * Return: The bio mapping result (DM_MAPIO_REMAPPED or DM_MAPIO_SUBMITTED)
 */
static int process_bio_locked(struct cache_block *cb,
                              struct bio         *bio,
                              struct bio_list    *ready)
{
  struct lossy_device *ld = cb->device;
  sector_t block_number = bio->bi_iter.bi_sector >> ld->block_shift;

  if (cb->state == EMPTY) {
    if ((bio_data_dir(bio) == READ)
        || (bio->bi_opf & REQ_FUA)
        || (bio_op(bio) == REQ_OP_DISCARD)
        || (bio->bi_iter.bi_size < ld->block_size))
      return DM_MAPIO_REMAPPED;

    /* Skip the cache if this address isn't covered by the mask */
    if ((ld->block_mask & (1 << (block_number % ld->mask_length))) == 0)
      return DM_MAPIO_REMAPPED;

    atomic_inc(&ld->busy_count);
    cb->block_number = block_number;
    process_cached_bio(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (cb->block_number != block_number) {
    return DM_MAPIO_REMAPPED;
  };

  if (cb->state == WRITING) {
    bio_list_add(&cb->pending_bios, bio);
    return DM_MAPIO_SUBMITTED;
  } else if (!(bio->bi_opf & REQ_FUA) && (bio_op(bio) != REQ_OP_DISCARD)) {
    process_cached_bio(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (bio->bi_iter.bi_size == ld->block_size) {
    cb->state = EMPTY;
    /* This decrement can never drop the busy count to zero */
    atomic_dec(&ld->busy_count);
    return DM_MAPIO_REMAPPED;
  } else {
    bio_list_add(&cb->pending_bios, bio);
    flush_cache_block(cb);
    return DM_MAPIO_SUBMITTED;
  }
}

/**
 * flush_cache() - Flush all of the cached data.
 * @ld: The lossy device
 */
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

/**
 * process_bio() - Process an incoming bio.
 * @ld: The lossy device
 * @bio: The bio to be processed
 * @ready: The list of unblocked bios that should be handled after returning
 *
 * Return: The bio mapping result (DM_MAPIO_REMAPPED or DM_MAPIO_SUBMITTED)
 */
static int process_bio(struct lossy_device *ld, struct bio *bio,
                       struct bio_list *ready)
{
  int result;

  if ((bio_data_dir(bio) == WRITE) && ld->stopped) {
    /* If we are stopped, treat all writes as errors */
    atomic64_inc(&ld->write_failures);
    bio->bi_status = ld->error_status;
    bio_endio(bio);
    return DM_MAPIO_SUBMITTED;
  }

  if (ld->cache_block_count == 0)
    return DM_MAPIO_REMAPPED;

  atomic_inc(&ld->busy_count);
  spin_lock_irq(&ld->flush_lock);
  if ((bio_op(bio) == REQ_OP_FLUSH) || (bio->bi_opf & REQ_PREFLUSH)) {
    bool flushing;

    if (bio->bi_iter.bi_size > 0)
      DMWARN("flush bio too big!");

    bio_list_add(&ld->flush_bios, bio);
    flushing = ld->flushing;
    ld->flushing = true;
    spin_unlock_irq(&ld->flush_lock);
    if (!flushing)
      flush_cache(ld);
    result = DM_MAPIO_SUBMITTED;
  } else if (ld->flushing) {
    bio_list_add(&ld->pending_bios, bio);
    spin_unlock_irq(&ld->flush_lock);
    result = DM_MAPIO_SUBMITTED;
  } else {
    sector_t block_number;
    u16 slotNumber;
    struct cache_block *cb;

    spin_unlock_irq(&ld->flush_lock);
    block_number = bio->bi_iter.bi_sector >> ld->block_shift;
    slotNumber = block_number % ld->cache_block_count;
    cb = &ld->cache_blocks[slotNumber];
    spin_lock_irq(&cb->lock);
    result = process_bio_locked(cb, bio, ready);
    spin_unlock_irq(&cb->lock);
  }

  complete_flushes(ld);
  return result;
}

/**
 * process_ready_bios() - Process a list of delayed bios.
 * @ld: The lossy device
 * @ready: bio list of bios that are ready to process
 */
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

static void free_cache_blocks(struct lossy_device *ld)
{
  u16 i;

  vfree(ld->cache_data);

  for (i = 0; i < ld->cache_block_count; i++) {
    struct cache_block *cb = &ld->cache_blocks[i];

    if (cb->bio != NULL) {
      bio_uninit(cb->bio);
      kfree(cb->bio);
    }
  }
}

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

static void lossy_dtr(struct dm_target *ti)
{
  struct lossy_device *ld = ti->private;

  dm_put_device(ti, ld->dev);
  free_cache_blocks(ld);
}

static int lossy_prepare_ioctl(struct dm_target     *ti,
                               struct block_device **bdev,
                               unsigned int          cmd,
                               unsigned long         arg,
                               bool                 *forward)
{
  struct dm_dev *dev = ((struct lossy_device *) ti->private)->dev;

  *bdev = dev->bdev;

  /* Only pass ioctls through if the device sizes match exactly */
  if (ti->len != bdev_nr_bytes(dev->bdev) >> SECTOR_SHIFT)
    return 1;

  return 0;
}

static int lossy_iterate_devices(struct dm_target           *ti,
                                 iterate_devices_callout_fn  fn,
                                 void                       *data)
{
  struct dm_dev *dev = ((struct lossy_device *) ti->private)->dev;

  return fn(ti, dev, 0, ti->len, data);
}

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

static int lossy_map(struct dm_target *ti,
                     struct bio       *bio)
{
  struct lossy_device *ld = ti->private;
  struct bio_list ready_bios;
  int result;

  bio_set_dev(bio, ld->dev->bdev);
  bio->bi_iter.bi_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);

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

  bio_list_init(&ready_bios);
  result = process_bio(ld, bio, &ready_bios);
  process_ready_bios(ld, &ready_bios);

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
  unsigned int sz = 0;

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

static struct target_type lossy_target_type = {
  .name            = "lossy",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = lossy_ctr,
  .dtr             = lossy_dtr,
  .iterate_devices = lossy_iterate_devices,
  .map             = lossy_map,
  .message         = lossy_message,
  .prepare_ioctl   = lossy_prepare_ioctl,
  .presuspend      = lossy_presuspend,
  .status          = lossy_status,
};

int __init lossy_init(void)
{
  int result = dm_register_target(&lossy_target_type);

  if (result < 0)
    DMERR("dm_register_target failed %d", result);
  return result;
}

void __exit lossy_exit(void)
{
  dm_unregister_target(&lossy_target_type);
}

module_init(lossy_init);
module_exit(lossy_exit);

MODULE_DESCRIPTION(DM_NAME " lossy testing device");
MODULE_AUTHOR("Matthew Sakai <dm-devel@lists.linux.dev>");
MODULE_LICENSE("GPL");
