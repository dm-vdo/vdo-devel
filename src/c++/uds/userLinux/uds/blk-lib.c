/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/blkdev.h>
#include <linux/err.h>

#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "errors.h"

int blkdev_issue_zeroout(struct block_device *bdev,
                         sector_t sector,
                         sector_t nr_sects,
                         gfp_t gfp_mask __always_unused,
                         unsigned flags __always_unused)
{
  int result;
  off_t offset, length, file_size;
  u8 *buffer;

  offset = sector * SECTOR_SIZE;
  length = nr_sects * SECTOR_SIZE;
  file_size = bdev->size;

  if ((offset + length) > file_size) {
    return UDS_OUT_OF_RANGE;
  }

  result = vdo_allocate(length, __func__, &buffer);
  if (result != VDO_SUCCESS)
    return result;

  memset(buffer, 0, length);

  result = write_buffer_at_offset(bdev->fd, offset, buffer, length);
  if (result != UDS_SUCCESS) {
    vdo_free(vdo_forget(buffer));
    return result;
  }

  result = logging_fsync(bdev->fd, "zero out");
  vdo_free(vdo_forget(buffer));
  return result;
}
