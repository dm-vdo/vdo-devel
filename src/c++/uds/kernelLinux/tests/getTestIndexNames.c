// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

#include <linux/version.h>

#undef VDO_USE_ALTERNATE
#undef VDO_USE_ALTERNATE_2
#undef VDO_USE_ALTERNATE_3
#ifdef RHEL_RELEASE_CODE
#define VDO_USE_ALTERNATE
#define VDO_USE_ALTERNATE_2
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 4))
#define VDO_USE_ALTERNATE_3
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,9,0))
#define VDO_USE_ALTERNATE
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,7,0))
#define VDO_USE_ALTERNATE_2
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,5,0))
#define VDO_USE_ALTERNATE_3
#endif
#endif
#endif
#endif /* !RHEL_RELEASE_CODE */

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/**********************************************************************/
static struct block_device *get_device_from_name(const char *name)
{
	struct block_device *bdev;

#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3

	bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
#else
	const struct blk_holder_ops hops = { NULL };

	bdev = blkdev_get_by_path(name, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE_3 */
#else
	const struct blk_holder_ops hops = { NULL };
	struct bdev_handle *bdev_handle;

	bdev_handle = bdev_open_by_path(name, BLK_FMODE, NULL, &hops);
	bdev = bdev_handle->bdev;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	const struct blk_holder_ops hops = { NULL };
	struct file *file_handle;
	
	file_handle = bdev_file_open_by_path(name, BLK_FMODE, NULL, &hops);
	bdev = file_bdev(file_handle);
	get_device(&bdev->bd_device);
	fput(file_handle);
#endif /* VDO_USE_ALTERNATE */

	if (IS_ERR(bdev)) {
		vdo_log_error_strerror(-PTR_ERR(bdev), "%s is not a block device", name);
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
#if defined(VDO_USE_ALTERNATE) && !defined(VDO_USE_ALTERNATE_2)
	struct bdev_handle bdev_handle = { .bdev = bdev, };

#endif /* VDO_USE_ALTERNATE && VDO_USE_ALTERNATE_2 */
	if (bdev == NULL)
		return;

#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
	blkdev_put(bdev, BLK_FMODE);
#else
	blkdev_put(bdev, NULL);
#endif /* VDO_USE_ALTERNATE_3 */
#else
	bdev_release(&bdev_handle);
#endif /* VDO_USE_ALTERNATE_2 */
#else
	put_device(&bdev->bd_device);
#endif /* VDO_USE_ALTERNATE */
}
