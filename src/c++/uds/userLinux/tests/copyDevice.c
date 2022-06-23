/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "assertions.h"
#include "numeric.h"
#include "testPrototypes.h"

int copyDevice(const char *source, const char *destination, off_t bytes)
{
  enum {
    SECTOR_SIZE = 512,
  };
  int read_fd;
  int write_fd;
  byte buffer[SECTOR_SIZE];
  off_t offset;
  off_t file_size;
  size_t length;

  UDS_ASSERT_SUCCESS(open_file(source, FU_READ_WRITE, &read_fd));
  UDS_ASSERT_SUCCESS(get_open_file_size(read_fd, &file_size));

  UDS_ASSERT_SUCCESS(open_file(destination, FU_CREATE_READ_WRITE, &write_fd));

  file_size = min(file_size, bytes);

  for (offset = 0; offset < file_size; ) {
    UDS_ASSERT_SUCCESS(read_data_at_offset(read_fd, offset,
                                           buffer, SECTOR_SIZE, &length));
    UDS_ASSERT_SUCCESS(write_buffer(write_fd, buffer, length));
    offset += length;
  }

  UDS_ASSERT_SUCCESS(sync_and_close_file(write_fd, "device copy write"));
  UDS_ASSERT_SUCCESS(close_file(read_fd, "device copy read"));
  return UDS_SUCCESS;
}
