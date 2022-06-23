// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
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

  if (client == NULL) {
    return -ENOMEM;
  }

  read_bdev = blkdev_get_by_path(source, FMODE_READ, NULL);
  write_bdev = blkdev_get_by_path(destination, FMODE_WRITE, NULL);
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

  blkdev_put(read_bdev, FMODE_READ);
  blkdev_put(write_bdev, FMODE_WRITE);
  dm_kcopyd_client_destroy(client);
  return kcopydResult.result;
}
