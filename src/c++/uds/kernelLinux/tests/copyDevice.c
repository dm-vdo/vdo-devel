// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/completion.h>
#include <linux/dm-kcopyd.h>
#include <linux/kernel.h>

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

int copyDevice(struct block_device *source,
	       struct block_device *destination,
	       off_t bytes)
{
  sector_t count;
  struct dm_io_region from;
  struct dm_io_region to[1];
  KcopydResult kcopydResult;
  struct dm_kcopyd_client *client = dm_kcopyd_client_create(NULL);

  if (client == NULL) {
    return -ENOMEM;
  }

  count = min(bdev_nr_sectors(source), bdev_nr_sectors(destination));
  count = min(count, (sector_t) ((bytes + SECTOR_SIZE - 1) >> SECTOR_SHIFT));

  from = (struct dm_io_region) {
    .bdev = source,
    .sector = 0,
    .count = count,
  };

  to[0] = (struct dm_io_region) {
    .bdev = destination,
    .sector = 0,
    .count = count,
  };

  init_completion(&kcopydResult.completion);
  dm_kcopyd_copy(client, &from, 1, to, 0, copyCallback, &kcopydResult);
  wait_for_completion(&kcopydResult.completion);
  dm_kcopyd_client_destroy(client);
  return kcopydResult.result;
}
