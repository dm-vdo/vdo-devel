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
#include <unistd.h>

#include "constants.h"

/*
 * Very simple program to warm-up VDO's block cache.  It reads bytes,
 * skipping a block map page each time.  This causes VDO to read its block map
 * cache into memory (assuming VDO has been started with enough block map cache
 * to keep it entirely resident in RAM, and assuming the tree is allocated).
 */

const off_t span = VDO_BLOCK_MAP_ENTRIES_PER_PAGE * VDO_BLOCK_SIZE;

int main(int argc, char **argv)
{
  if (argc != 2) {
    err(1, "USAGE vdoWarmup /dev/my_device");
    return 1;
  }

  int fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    err(1, "Unable to open %s", argv[1]);
  }

  char byte;
  size_t reads = 0;

  // Just keep reading and seeking until we get an error.
  while (read(fd, &byte, 1) == 1) {
    reads++;
    if (lseek(fd, span - 1, SEEK_CUR) < 0) {
      break;
    }
  }

  printf("Normal exit at end of file after %zu reads", reads);
  return 0;
}
