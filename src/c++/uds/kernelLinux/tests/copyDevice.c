// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/completion.h>
#include <linux/dm-kcopyd.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include "testPrototypes.h"

typedef struct {
  struct completion completion;
  int result;
} KcopydResult;
#undef VDO_USE_ALTERNATE
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
#define VDO_USE_ALTERNATE
#endif

static void copyCallback(int readErr, unsigned long writeErr, void *context)
{
  KcopydResult *result = context;
  result->result = (((readErr != 0) || (writeErr != 0)) ? -EIO : UDS_SUCCESS);
  complete(&result->completion);
}

int copyDevice(const char *source, const char *destination, off_t bytes)
{
  struct block_device *read_bdev;
  struct block_device *write_bdev;
  sector_t count;
  struct dm_io_region from;
  struct dm_io_region to[1];
  KcopydResult kcopydResult;
  struct dm_kcopyd_client *client = dm_kcopyd_client_create(NULL);
#ifndef VDO_USE_ALTERNATE
  const struct blk_holder_ops hops = { NULL };
#endif

  if (client == NULL) {
    return -ENOMEM;
  }

#ifdef VDO_USE_ALTERNATE
  read_bdev = blkdev_get_by_path(source, FMODE_READ, NULL);
  write_bdev = blkdev_get_by_path(destination, FMODE_WRITE, NULL);
#else
  read_bdev = blkdev_get_by_path(source, FMODE_READ, NULL, &hops);
  write_bdev = blkdev_get_by_path(destination, FMODE_WRITE, NULL, &hops);
#endif  
  count = min(bdev_nr_sectors(read_bdev), bdev_nr_sectors(write_bdev));
  count = min(count, (sector_t) ((bytes + SECTOR_SIZE - 1) >> SECTOR_SHIFT));

  from = (struct dm_io_region) {
    .bdev = read_bdev,
    .sector = 0,
    .count = count,
  };

  to[0] = (struct dm_io_region) {
    .bdev = write_bdev,
    .sector = 0,
    .count = count,
  };

  init_completion(&kcopydResult.completion);
  dm_kcopyd_copy(client, &from, 1, to, 0, copyCallback, &kcopydResult);
  wait_for_completion(&kcopydResult.completion);

#ifdef VDO_USE_ALTERNATE
  blkdev_put(read_bdev, FMODE_READ);
  blkdev_put(write_bdev, FMODE_WRITE);
#else
  blkdev_put(read_bdev, NULL);
  blkdev_put(write_bdev, NULL);
#endif /* VDO_USE_ALTERNATE */
  dm_kcopyd_client_destroy(client);
  return kcopydResult.result;
}
