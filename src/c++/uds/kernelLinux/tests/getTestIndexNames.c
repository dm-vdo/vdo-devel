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
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_MINOR) && (RHEL_MINOR < 50)
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

struct block_device_context {
	struct block_device *block_device;
#ifdef VDO_USE_ALTERNATE
#ifndef VDO_USE_ALTERNATE_2
	struct bdev_handle *device_handle;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	struct file *file_handle;
#endif /* VDO_USE_ALTERNATE */
};

static struct block_device_context contexts[2];

/**********************************************************************/
static void set_device_context(int context_number, const char *name)
{
	struct block_device_context *context = &contexts[context_number];
#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3

	context->block_device = blkdev_get_by_path(name, BLK_FMODE, NULL);
#else
	static const struct blk_holder_ops hops = { NULL };

	context->block_device = blkdev_get_by_path(name, BLK_FMODE, NULL, &hops);
#endif /* VDO_USE_ALTERNATE_3 */
#else
	static const struct blk_holder_ops hops = { NULL };
	struct bdev_handle *bdev_handle;

	bdev_handle = bdev_open_by_path(name, BLK_FMODE, NULL, &hops);
	if (IS_ERR(bdev_handle)) {
		vdo_log_error_strerror(-PTR_ERR(bdev_handle),
				       "cannot get device handle for %s", name);
		context->block_device = NULL;
		return;
	}

	context->device_handle = bdev_handle;
	context->block_device = bdev_handle->bdev;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	static const struct blk_holder_ops hops = { NULL };
	struct file *file_handle;

	file_handle = bdev_file_open_by_path(name, BLK_FMODE, NULL, &hops);
	if (IS_ERR(file_handle)) {
		vdo_log_error_strerror(-PTR_ERR(file_handle),
				       "cannot get file handle for %s", name);
		context->block_device = NULL;
		return;
	}

	context->file_handle = file_handle;
	context->block_device = file_bdev(file_handle);
#endif /* VDO_USE_ALTERNATE */

	if (IS_ERR(context->block_device)) {
		vdo_log_error_strerror(-PTR_ERR(context->block_device),
				       "%s is not a block device", name);
		context->block_device = NULL;
	}
}

/**********************************************************************/
struct block_device *getTestBlockDevice(void)
{
	set_device_context(0, "/dev/zubenelgenubi_scratch");
	return contexts[0].block_device;
}

/**********************************************************************/
struct block_device **getTestMultiBlockDevices(void)
{
	static struct block_device *bdevs[2];

	set_device_context(0, "/dev/zubenelgenubi_scratch-0");
	set_device_context(1, "/dev/zubenelgenubi_scratch-1");
	bdevs[0] = contexts[0].block_device;
	bdevs[1] = contexts[1].block_device;
	return bdevs;
}

/**********************************************************************/
static void close_device_context(struct block_device_context *context)
{
#ifdef VDO_USE_ALTERNATE
#ifdef VDO_USE_ALTERNATE_2
#ifdef VDO_USE_ALTERNATE_3
	blkdev_put(context->block_device, BLK_FMODE);
#else
	blkdev_put(context->block_device, NULL);
#endif /* VDO_USE_ALTERNATE_3 */
#else
	bdev_release(context->device_handle);
	context->device_handle = NULL;
#endif /* VDO_USE_ALTERNATE_2 */
#else
	fput(context->file_handle);
	context->file_handle = NULL;
#endif /* VDO_USE_ALTERNATE */
	context->block_device = NULL;
}

/**********************************************************************/
void putTestBlockDevice(struct block_device *bdev)
{
	int n;

	if (bdev == NULL)
		return;

	for (n = 0; n < 2; n++) {
		if (contexts[n].block_device == bdev) {
			close_device_context(&contexts[n]);
			return;
		}
	}

	vdo_log_error("block device freed but not opened");
}
