/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testPrototypes.h"

#include "assertions.h"
#include "fileUtils.h"
#include "numeric.h"

int copyDevice(struct block_device *source,
               struct block_device *destination,
               off_t bytes)
{
  u8 buffer[SECTOR_SIZE];
  off_t offset;
  off_t file_size;
  size_t length;

  UDS_ASSERT_SUCCESS(get_open_file_size(source->fd, &file_size));
  file_size = min(file_size, bytes);

  for (offset = 0; offset < file_size; ) {
    UDS_ASSERT_SUCCESS(read_data_at_offset(source->fd, offset,
                                           buffer, SECTOR_SIZE, &length));
    UDS_ASSERT_SUCCESS(write_buffer(destination->fd, buffer, length));
    offset += length;
  }

  UDS_ASSERT_SUCCESS(logging_fsync(destination->fd, "device copy write"));
  return UDS_SUCCESS;
}
