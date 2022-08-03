/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 * Copyright Red Hat
 *
 */

#ifndef _LINUX_DEVICE_MAPPER_H
#define _LINUX_DEVICE_MAPPER_H

#include <linux/kobject.h>

#include "linuxTypes.h"

#define SECTOR_SHIFT 9

struct dm_target;
struct dm_table;
struct mapped_device;

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

#endif	/* _LINUX_DEVICE_MAPPER_H */
