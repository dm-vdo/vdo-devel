// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/blkdev.h>
#include <linux/version.h>

#include "errors.h"
#include "fileUtils.h"
#include "memory-alloc.h"

#undef VDO_USE_ALTERNATE
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
#define VDO_USE_ALTERNATE
#endif

#ifdef VDO_USE_ALTERNATE
struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder __always_unused)
#else
struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder __always_unused,
					const struct blk_holder_ops *hops __always_unused)
#endif /* VDO_USE_ALTERNATE */
{
	int result;
	int fd;
	struct block_device *device;

	if (mode != (FMODE_READ | FMODE_WRITE))
		return (struct block_device *) -EACCES;

	result = open_file(path, FU_READ_WRITE, &fd);
	if (result != UDS_SUCCESS)
		return (struct block_device *) (int64_t) -result;

	result = UDS_ALLOCATE(1, struct block_device, __func__, &device);
	if (result != UDS_SUCCESS) {
		close_file(fd, NULL);
		return (struct block_device *) -ENOMEM;
	}

	device->fd = fd;
	return device;
}

#ifdef VDO_USE_ALTERNATE
void blkdev_put(struct block_device *bdev, fmode_t mode __always_unused)
#else
void blkdev_put(struct block_device *bdev, void *holder __always_unused)
#endif /* VDO_USE_ALTERNATE */
{
	close_file(bdev->fd, NULL);
	UDS_FREE(bdev);
}
