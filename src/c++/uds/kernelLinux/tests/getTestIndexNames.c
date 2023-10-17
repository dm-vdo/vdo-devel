// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

#include <linux/version.h>

#undef VDO_USE_ALTERNATE
#ifdef RHEL_RELEASE_CODE
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 4))
#define VDO_USE_ALTERNATE
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0))
#define VDO_USE_ALTERNATE
#endif
#endif /* !RHEL_RELEASE_CODE */

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/**********************************************************************/
static struct block_device *get_device_from_name(const char *name)
{
	struct block_device *bdev;
#ifdef VDO_USE_ALTERNATE

	bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
#else
	const struct blk_holder_ops hops = { NULL };

	bdev = blkdev_get_by_path(name, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE */

	if (IS_ERR(bdev)) {
		uds_log_error_strerror(-PTR_ERR(bdev), "%s is not a block device", name);
		return NULL;
	}

	return bdev;
}

/**********************************************************************/
struct block_device *getTestBlockDevice(void)
{
	return get_device_from_name("/dev/zubenelgenubi_scratch");
}

/**********************************************************************/
struct block_device **getTestMultiBlockDevices(void)
{
	static struct block_device *bdevs[2];

	bdevs[0] = get_device_from_name("/dev/zubenelgenubi_scratch-0");
	bdevs[1] = get_device_from_name("/dev/zubenelgenubi_scratch-1");
	return bdevs;
}

/**********************************************************************/
void putTestBlockDevice(struct block_device *bdev)
{
	if (bdev == NULL)
		return;

#ifdef VDO_USE_ALTERNATE
	blkdev_put(bdev, BLK_FMODE);
#else
	blkdev_put(bdev, NULL);
#endif /* VDO_USE_ALTERNATE */
}
