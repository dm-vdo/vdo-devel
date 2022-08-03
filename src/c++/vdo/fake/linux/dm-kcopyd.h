/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kcopyd provides a simple interface for copying an area of one
 * block-device to one or more other block-devices, either synchronous
 * or with an asynchronous completion notification.
 *
 * The functions declared here are only defined in tests/dm-kcopyd.c, as they
 * are using VDO completions to implement.
 *
 * Copyright (C) 2001 - 2003 Sistina Software
 * Copyright Red Hat
 *
 */

#ifndef _LINUX_DM_KCOPYD_H
#define _LINUX_DM_KCOPYD_H

#include "linuxTypes.h"

struct dm_io_region {
	struct block_device *bdev;
	sector_t sector;
	sector_t count;		/* If this is zero the region is ignored. */
};

struct dm_kcopyd_client;
struct dm_kcopyd_throttle;
struct dm_kcopyd_client *dm_kcopyd_client_create(struct dm_kcopyd_throttle *throttle);
void dm_kcopyd_client_destroy(struct dm_kcopyd_client *kc);

typedef void (*dm_kcopyd_notify_fn)(int read_err, unsigned long write_err,
				    void *context);

/*
 * This mock cheats and requires context to be a vdo_completion so it can get
 * its VDO field to make more completions..
 */
void dm_kcopyd_copy(struct dm_kcopyd_client *kc, struct dm_io_region *from,
		    unsigned num_dests, struct dm_io_region *dests,
		    unsigned flags, dm_kcopyd_notify_fn fn, void *context);

void dm_kcopyd_zero(struct dm_kcopyd_client *kc,
		    unsigned num_dests, struct dm_io_region *dests,
		    unsigned flags, dm_kcopyd_notify_fn fn, void *context);

#endif	/* _LINUX_DM_KCOPYD_H */
