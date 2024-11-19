/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/err.h>
#include <linux/kernel.h>

#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "status-codes.h"

int blkdev_issue_zeroout(struct block_device *bdev,
                         sector_t sector,
                         sector_t nr_sects,
                         __attribute__((unused)) gfp_t gfp_mask,
                         __attribute__((unused)) unsigned flags)
{
  int result;
  off_t offset, length, file_size;
  void *buffer;

  offset = sector * SECTOR_SIZE;
  length = nr_sects * SECTOR_SIZE;
  file_size = bdev->size;

  result = vdo_allocate(length, u8, __func__, &buffer);
  if (result != VDO_SUCCESS)
    return result;

  memset(buffer, 0, length);

  if ((offset + length) > file_size) {
    vdo_free(vdo_forget(buffer));
    return VDO_OUT_OF_RANGE;
  }

  result = write_buffer_at_offset(bdev->fd, offset, buffer, length);
  if (result != VDO_SUCCESS) {
    vdo_free(vdo_forget(buffer));
    return result;
  }

  result = logging_fsync(bdev->fd, "zero out");
  vdo_free(vdo_forget(buffer));
  return result;
}

