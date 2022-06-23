/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**********************************************************************/
static void usage(void)
{
  fprintf(stdout, "setReadOnly [device] [0 or 1]\n");
}

/**********************************************************************/
static int getMode(const char *arg)
{
  char *leftover;
  long value = strtol(arg, &leftover, 0);
  if (leftover[0] != '\0') {
    err(2, "Mode must be 0 or 1");
  }
  if ((value != 0) && (value != 1)) {
    err(2, "Mode must be 0 or 1");
  }
  return (int) value;
}

/**********************************************************************/
int main(int argc, char **argv)
{
  if (argc < 3) {
    usage();
    exit(2);
  }

  int mode = getMode(argv[2]);
  const char *path = argv[1];

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    err(2, "open failure on %s", path);
  }

  if (ioctl(fd, BLKROSET, &mode) != 0) {
    err(2, "ioctl failure on %s", path);
  }

  if (close(fd) != 0) {
    err(2, "close failure on %s", path);
  }
  return 0;
}
