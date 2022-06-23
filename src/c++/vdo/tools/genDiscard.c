/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**********************************************************************/
static int numConvert(const char *arg)
{
  char *leftover;
  long value = strtol(arg, &leftover, 0);
  if (leftover[0] != '\0') {
    if (leftover[1] != '\0') {
      errx(2, "Invalid number");
    }
    if (value != (int) value) {
      errx(2, "Numeric value too large");
    }
    switch (leftover[0]) {
    default:
      errx(2, "Invalid number");
    case 'G':
    case 'g':
      value *= 1024;
      // fall thru
    case 'M':
    case 'm':
      value *= 1024;
      // fall thru
    case 'K':
    case 'k':
      value *= 1024;
      break;
    }
  }
  if (value != (int) value) {
    errx(2, "Numeric value too large");
  }
  return (int) value;
}

/**********************************************************************/
static void usage(int verbose)
{
  fprintf(stderr,
          "Usage:  genDiscard [--help] [--bs=N] [--count=N] --of=<path>\n"
          "                   [--sync] [--seek=N] [--stride=N] [--zero]\n"
          "                   [--fraction=N] [--seed=N]\n");
  if (verbose) {
    fprintf(stderr,
            "\n"
            "\t--bs=N        sets the blocksize to N (default 4K)\n"
            "\n"
            "\t--count=N     sets the block count to trim to N (default 1)\n"
            "\n"
            "\t--seek=N      skips over N blocks before trimming (default 0)\n"
            "\n"
            "\t--sync        issues an fdatasync on the file before closing\n"
            "\n"
            "\t--of=Path     sets the pathname of the device\n"
            "\n"
            "\t--stride=N    when non-zero, iteratively discards successive\n"
            "\t              chunks of N blocks (default 0)\n"
            "\n"
            "\t--zero        zeros blocks instead of discarding them, which\n"
            "\t              allows the use of a file instead of a device\n"
            "\n"
            "\t--fraction=N  randomly discards chunks, where N is the\n"
            "\t              expected fraction of chunks to discard\n"
            "\t              (default 1.0)\n"
            "\n"
            "\t--seed=N      sets the random number seed\n"
            "\t              (default is the current time)\n"
            "\n"
            "\t--help        prints this help\n");
  }
  exit(verbose ? 0 : 1);
}

/**********************************************************************/
int main(int argc, char **argv)
{
  char  *path       = NULL;
  off_t  blockSize  = 4096;
  int    count      = 1;
  float  fraction   = 1.0;
  int    seed       = time(NULL);
  int    seek       = 0;
  int    stride     = 0;
  bool   sync       = false;
  bool   zeroChunks = false;

  struct option options[] = {
    { "bs",       required_argument,  NULL, 'b' },
    { "count",    required_argument,  NULL, 'c' },
    { "fraction", required_argument,  NULL, 'f' },
    { "help",     no_argument,        NULL, 'h' },
    { "of",       required_argument,  NULL, 'o' },
    { "seed",     required_argument,  NULL, 'S' },
    { "seek",     required_argument,  NULL, 's' },
    { "stride",   required_argument,  NULL, 'n' },
    { "sync  ",   optional_argument,  NULL, 'y' },
    { "zero",     optional_argument,  NULL, 'z' },
    { NULL,       0,                  NULL,  0  },
  };
  int opt;
  while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
    case 'b':  // --bs=<block_size>
      blockSize = numConvert(optarg);
      break;
    case 'c': // --count=<count>
      count = numConvert(optarg);
      break;
    case 'f': // --fraction=<percent>
      fraction = atof(optarg);
      break;
    case 'h': // --help
      usage(1);
      break;
    case 'o': // --of=<path>
      path = optarg;
      break;
    case 'S': // --seed=<value>
      seed = numConvert(optarg);
      break;
    case 's': // --seek=<count>
      seek = numConvert(optarg);
      break;
    case 'n': // --stride=<count>
      stride = numConvert(optarg);
      break;
    case 'y': // --sync[=<n>]
      sync = ((optarg == NULL) ? false : numConvert(optarg));
      break;
    case 'z': // --zero[=<n>]
      zeroChunks = ((optarg == NULL) ? true : numConvert(optarg));
      break;
    default:
      usage(0);
      break;
    }
  }
  if (optind < argc) {
    errx(3, "optind %d argc %d", optind, argc);
    usage(0);
  }

  if (path == NULL) {
    errx(3, "the device path must be provided");
  }

  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    err(3, "open failure on %s", path);
  }

  if (stride <= 0) {
    stride = count;
  } else if (stride > count) {
    errx(3, "stride size must not exceed the block count");
  }

  srandom(seed);

  char *zeros = NULL;
  if (zeroChunks) {
    zeros = calloc(stride, blockSize);
  }

  int    nextBlock         = seek;
  int    remaining         = count;
  size_t discardBlockCount = 0;
  size_t discardCallCount  = 0;

  while (remaining > 0) {
    // The block count might not be a multiple of the stride size, so make sure
    // not to trim beyond the end of the specified range.
    if (stride > remaining) {
      stride = remaining;
    }

    if ((fraction >= 1.0) || (random() < (fraction * RAND_MAX))) {
      uint64_t range[2] = { nextBlock * blockSize, stride * blockSize };
      if (zeroChunks) {
        int64_t result = pwrite(fd, zeros, range[1], range[0]);
        if (result < 0) {
          err(3, "pwrite failure on %s", path);
        }
        if ((uint64_t) result != range[1]) {
          errx(3, "short write on %s", path);
        }
      } else {
        if (ioctl(fd, BLKDISCARD, range) != 0) {
          err(3, "ioctl failure on %s", path);
        }
      }
      discardBlockCount += stride;
      discardCallCount  += 1;
    }

    nextBlock += stride;
    remaining -= stride;
  }

  if (sync && (fdatasync(fd) != 0)) {
    err(3, "fsyncdata failure on %s", path);
  }

  if (close(fd) != 0) {
    err(3, "close failure on %s", path);
  }

  free(zeros);

  printf("genDiscard %sed %zu block%s in %zu operation%s\n",
         (zeroChunks ? "zero" : "discard"),
         discardBlockCount, ((discardBlockCount != 1) ? "s" : ""),
         discardCallCount, ((discardCallCount != 1) ? "s" : ""));
  return 0;
}
