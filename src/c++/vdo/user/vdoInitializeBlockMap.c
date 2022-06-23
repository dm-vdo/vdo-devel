/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"

/*
 * Very simple program to initialize VDO's block map.  It writes bytes,
 * skipping a block map page each time.  This causes VDO to allocate all
 * block map pages in memory, assuming VDO has enough physical space.
 */
const size_t SPAN = VDO_BLOCK_MAP_ENTRIES_PER_PAGE * VDO_BLOCK_SIZE;

int main(int argc, char **argv)
{
  if (argc != 2) {
    err(1, "USAGE vdoInitializeBlockMap /dev/my_device");
    return 1;
  }

  int fd = open(argv[1], O_WRONLY);
  if (fd == -1) {
    err(1, "Unable to open %s", argv[1]);
  }

  char zeros[VDO_BLOCK_SIZE];
  memset(zeros, 0, sizeof(zeros));

  size_t writes = 0;

  while (write(fd, zeros, sizeof(zeros)) == ((int) sizeof(zeros))) {
    writes++;
    if (lseek(fd, SPAN - sizeof(zeros), SEEK_CUR) < 0) {
      break;
    }
  }

  if (fsync(fd) != 0) {
    err(3, "fsync failure on %s", argv[1]);
  }

  printf("Normal exit at end of file after %zu writes", writes);
  return 0;
}
