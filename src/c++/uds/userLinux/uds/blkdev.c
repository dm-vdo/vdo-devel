// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/blkdev.h>

#include "errors.h"
#include "fileUtils.h"
#include "memory-alloc.h"

struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder __always_unused)
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

void blkdev_put(struct block_device *bdev, fmode_t mode __always_unused)
{
	close_file(bdev->fd, NULL);
	UDS_FREE(bdev);
}
