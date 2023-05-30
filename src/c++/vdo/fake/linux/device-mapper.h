/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 * Copyright 2023 Red Hat
 *
 */

#ifndef _LINUX_DEVICE_MAPPER_H
#define _LINUX_DEVICE_MAPPER_H

#include <linux/blk_types.h>
#include <linux/kobject.h>

#include "linuxTypes.h"

#define SECTOR_SHIFT 9

/*
 * Definitions of return values from target end_io function.
 */
#define DM_ENDIO_DONE		0
#define DM_ENDIO_INCOMPLETE	1
#define DM_ENDIO_REQUEUE	2
#define DM_ENDIO_DELAY_REQUEUE	3

/*
 * Definitions of return values from target map function.
 */
#define DM_MAPIO_SUBMITTED	0
#define DM_MAPIO_REMAPPED	1
#define DM_MAPIO_REQUEUE	DM_ENDIO_REQUEUE
#define DM_MAPIO_DELAY_REQUEUE	DM_ENDIO_DELAY_REQUEUE
#define DM_MAPIO_KILL		4

struct dm_target;
struct dm_table;
struct mapped_device;

typedef enum { STATUSTYPE_INFO, STATUSTYPE_TABLE, STATUSTYPE_IMA } status_type_t;

/*
 * In the constructor the target parameter will already have the
 * table, type, begin and len fields filled in.
 */
typedef int (*dm_ctr_fn) (struct dm_target *target,
			  unsigned int argc, char **argv);

/*
 * The destructor doesn't need to free the dm_target, just
 * anything hidden ti->private.
 */
typedef void (*dm_dtr_fn) (struct dm_target *ti);

/*
 * The map function must return:
 * < 0: error
 * = 0: The target will handle the io by resubmitting it later
 * = 1: simple remap complete
 * = 2: The target wants to push back the io
 */
typedef int (*dm_map_fn) (struct dm_target *ti, struct bio *bio);
typedef int (*dm_clone_and_map_request_fn) (struct dm_target *ti,
					    struct request *rq,
					    union map_info *map_context,
					    struct request **clone);
typedef void (*dm_release_clone_request_fn) (struct request *clone,
					     union map_info *map_context);

/*
 * Returns:
 * < 0 : error (currently ignored)
 * 0   : ended successfully
 * 1   : for some reason the io has still not completed (eg,
 *       multipath target might want to requeue a failed io).
 * 2   : The target wants to push back the io
 */
typedef int (*dm_endio_fn) (struct dm_target *ti,
			    struct bio *bio, blk_status_t *error);
typedef int (*dm_request_endio_fn) (struct dm_target *ti,
				    struct request *clone, blk_status_t error,
				    union map_info *map_context);

typedef void (*dm_presuspend_fn) (struct dm_target *ti);
typedef void (*dm_presuspend_undo_fn) (struct dm_target *ti);
typedef void (*dm_postsuspend_fn) (struct dm_target *ti);
typedef int (*dm_preresume_fn) (struct dm_target *ti);
typedef void (*dm_resume_fn) (struct dm_target *ti);

typedef void (*dm_status_fn) (struct dm_target *ti, status_type_t status_type,
			      unsigned status_flags, char *result, unsigned maxlen);

typedef int (*dm_message_fn) (struct dm_target *ti, unsigned argc, char **argv,
			      char *result, unsigned maxlen);

typedef int (*dm_prepare_ioctl_fn) (struct dm_target *ti, struct block_device **bdev);

#ifdef CONFIG_BLK_DEV_ZONED
typedef int (*dm_report_zones_fn) (struct dm_target *ti,
				   struct dm_report_zones_args *args,
				   unsigned int nr_zones);
#else
/*
 * Define dm_report_zones_fn so that targets can assign to NULL if
 * CONFIG_BLK_DEV_ZONED disabled. Otherwise each target needs to do
 * awkward #ifdefs in their target_type, etc.
 */
typedef int (*dm_report_zones_fn) (struct dm_target *dummy);
#endif

/*
 * These iteration functions are typically used to check (and combine)
 * properties of underlying devices.
 * E.g. Does at least one underlying device support flush?
 *      Does any underlying device not support WRITE_SAME?
 *
 * The callout function is called once for each contiguous section of
 * an underlying device.  State can be maintained in *data.
 * Return non-zero to stop iterating through any further devices.
 */
typedef int (*iterate_devices_callout_fn) (struct dm_target *ti,
					   struct dm_dev *dev,
					   sector_t start, sector_t len,
					   void *data);

/*
 * This function must iterate through each section of device used by the
 * target until it encounters a non-zero return code, which it then returns.
 * Returns zero if no callout returned non-zero.
 */
typedef int (*dm_iterate_devices_fn) (struct dm_target *ti,
				      iterate_devices_callout_fn fn,
				      void *data);

typedef void (*dm_io_hints_fn) (struct dm_target *ti,
				struct queue_limits *limits);

/*
 * Returns:
 *    0: The target can handle the next I/O immediately.
 *    1: The target can't handle the next I/O immediately.
 */
typedef int (*dm_busy_fn) (struct dm_target *ti);

#ifdef __KERNEL__
/*
 * Returns:
 *  < 0 : error
 * >= 0 : the number of bytes accessible at the address
 */
typedef long (*dm_dax_direct_access_fn) (struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, enum dax_access_mode node, void **kaddr,
		pfn_t *pfn);
typedef int (*dm_dax_zero_page_range_fn)(struct dm_target *ti, pgoff_t pgoff,
		size_t nr_pages);

/*
 * Returns:
 * != 0 : number of bytes transferred
 * 0    : recovery write failed
 */
typedef size_t (*dm_dax_recovery_write_fn)(struct dm_target *ti, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i);
#endif /* __KERNEL__ */

void dm_error(const char *message);

struct dm_target {
	struct dm_table *table;
	struct target_type *type;

	/* target limits */
	sector_t begin;
	sector_t len;

	/* If non-zero, maximum size of I/O submitted to a target. */
	uint32_t max_io_len;

	/*
	 * A number of zero-length barrier bios that will be submitted
	 * to the target for the purpose of flushing cache.
	 *
	 * The bio number can be accessed with dm_bio_get_target_bio_nr.
	 * It is a responsibility of the target driver to remap these bios
	 * to the real underlying devices.
	 */
	unsigned num_flush_bios;

	/*
	 * The number of discard bios that will be submitted to the target.
	 * The bio number can be accessed with dm_bio_get_target_bio_nr.
	 */
	unsigned num_discard_bios;

	/*
	 * The number of secure erase bios that will be submitted to the target.
	 * The bio number can be accessed with dm_bio_get_target_bio_nr.
	 */
	unsigned num_secure_erase_bios;

	/*
	 * The number of WRITE SAME bios that will be submitted to the target.
	 * The bio number can be accessed with dm_bio_get_target_bio_nr.
	 */
	unsigned num_write_same_bios;

	/*
	 * The number of WRITE ZEROES bios that will be submitted to the target.
	 * The bio number can be accessed with dm_bio_get_target_bio_nr.
	 */
	unsigned num_write_zeroes_bios;

	/*
	 * The minimum number of extra bytes allocated in each io for the
	 * target to use.
	 */
	unsigned per_io_data_size;

	/* target specific data */
	void *private;

	/* Used to provide an error string from the ctr */
	char *error;

	/*
	 * Set if this target needs to receive flushes regardless of
	 * whether or not its underlying devices have support.
	 */
	bool flush_supported:1;

	/*
	 * Set if this target needs to receive discards regardless of
	 * whether or not its underlying devices have support.
	 */
	bool discards_supported:1;

	/*
	 * Set if we need to limit the number of in-flight bios when swapping.
	 */
	bool limit_swap_bios:1;
};

/*
 * Info functions.
 */
static inline const char *dm_device_name(struct mapped_device *md)
{
	return "fake device name";
}


static inline struct mapped_device *dm_table_get_md(struct dm_table *t) {
	return NULL;
}

/*
 * We need to be able to reference the bdev field of dm_dev in unit tests,
 * but it is fine for the field itself to be NULL.
 */
struct dm_dev {
  struct block_device *bdev;
};

// What follows are the most minimal implementations of things which are
// device-mapper adjacent.
struct device {
	struct kobject kobj;
};

/**********************************************************************/
static inline void
*dm_disk(struct mapped_device *device __attribute__((unused)))
{
	return NULL;
}

/**********************************************************************/
struct device * __must_check disk_to_dev(void *disk);

/**********************************************************************/
static inline unsigned long to_bytes(sector_t n)
{
	return (n << SECTOR_SHIFT);
}

/**********************************************************************/
void dm_put_device(struct dm_target *ti, struct dm_dev *d);

/**********************************************************************/
int dm_noflush_suspending(struct dm_target *ti);

int dm_get_device(struct dm_target *ti, const char *path, fmode_t mode,
		  struct dm_dev **result);

struct target_type {
	uint64_t features;
	const char *name;
	struct module *module;
	unsigned version[3];
	dm_ctr_fn ctr;
	dm_dtr_fn dtr;
	dm_map_fn map;
	dm_clone_and_map_request_fn clone_and_map_rq;
	dm_release_clone_request_fn release_clone_rq;
	dm_endio_fn end_io;
	dm_request_endio_fn rq_end_io;
	dm_presuspend_fn presuspend;
	dm_presuspend_undo_fn presuspend_undo;
	dm_postsuspend_fn postsuspend;
	dm_preresume_fn preresume;
	dm_resume_fn resume;
	dm_status_fn status;
	dm_message_fn message;
	dm_prepare_ioctl_fn prepare_ioctl;
	dm_report_zones_fn report_zones;
	dm_busy_fn busy;
	dm_iterate_devices_fn iterate_devices;
	dm_io_hints_fn io_hints;
#ifdef __KERNEL__
	dm_dax_direct_access_fn direct_access;
	dm_dax_zero_page_range_fn dax_zero_page_range;
	dm_dax_recovery_write_fn dax_recovery_write;
#endif /* __KERNEL__ */

	/* For internal device-mapper use. */
	struct list_head list;
};

/*
 * Any table that contains an instance of this target must have only one.
 */
#define DM_TARGET_SINGLETON		0x00000001

int dm_register_target(struct target_type *t);
void dm_unregister_target(struct target_type *t);

/*
 * Target argument parsing.
 */
struct dm_arg_set {
	unsigned argc;
	char **argv;
};

/*
 * Return the current argument and shift to the next.
 */
const char *dm_shift_arg(struct dm_arg_set *as);

/*
 * Move through num_args arguments.
 */
void dm_consume_args(struct dm_arg_set *as, unsigned num_args);

fmode_t dm_table_get_mode(struct dm_table *t);

#endif	/* _LINUX_DEVICE_MAPPER_H */
