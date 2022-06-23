/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/*
 * This is a command line program that is "like" the standard dd, but that
 * modifies the blocks being copied to have differing data with the same
 * murmur3 hash values.
 *
 * See https://131002.net/siphash/ for pointers to more information on
 * murmur3 collisions.
 *
 * See http://code.google.com/p/smhasher/wiki/MurmurHash3 for the sample
 * code that inspired this program.
 *
 * See https://131002.net/siphash/siphashdos_appsec12_slides.pdf to learn
 * how this program generates murmur3 hash collisions.
 *
 * Usage Hint #1 - How to generate two files that have no dedupe but have
 * the same murmur3 hashes:
 *    $ dd if=/dev/random of=first_file bs=4096 count=1000
 *    $ ./murmur3collide --if=first_file --of=second_file \
 *                       --bs=4096 --count=1000
 *
 * Usage Hint #2 - How to generate a single file with blocks that have no
 * dedupe but that all have the same murmur3 hashes:
 *    $ dd if=/dev/random of=col_file bs=4096 count=1
 *    $ ./murmur3collide --if=colliding_file --of=colliding_file \
 *                       --bs=4096 --count=999 --seek=1
 */

#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**********************************************************************/
static inline uint64_t rotl64(uint64_t x, int8_t r)
{
  return (x << r) | (x >> (64 - r));
}

/**********************************************************************/
static inline uint64_t endian_swap64(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return x;
#else
  return __builtin_bswap64(x);
#endif
}

/**********************************************************************/
static void m3forward(uint64_t chunk[2])
{
  // modify the chunk in place, performing the data-to-K transform that is
  // used by murmur3
  uint64_t k1 = endian_swap64(chunk[0]);
  uint64_t k2 = endian_swap64(chunk[1]);
  const uint64_t c1 = 0x87c37b91114253d5UL;
  const uint64_t c2 = 0x4cf5ad432745937fUL;
  k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2;
  k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1;
  chunk[0] = k1;
  chunk[1] = k2;
}

/**********************************************************************/
static void m3backward(uint64_t chunk[2])
{
  // modify the chunk in place, performing the inverse of the data-to-K
  // transform that is used by murmur3
  uint64_t k1 = chunk[0];
  uint64_t k2 = chunk[1];
  const uint64_t r1 = 0xa81e14edd9de2c7fUL;
  const uint64_t r2 = 0xa98409e882ce4d7dUL;
  k1 *= r1; k1 = rotl64(k1, 33); k1 *= r2;
  k2 *= r2; k2 = rotl64(k2, 31); k2 *= r1;
  chunk[0] = endian_swap64(k1);
  chunk[1] = endian_swap64(k2);
}

/**********************************************************************/
static void collide(unsigned char *block, off_t size)
{
  // Select a 32 byte chunk of the block.  So that Hint #2 (above) works,
  // use a Gray-code-like mechanism such that choosing a single block
  // produces new data with the same overall hash code.
  static unsigned long counter = 0;
  int index = 32 * (__builtin_ffsl(++counter) % (size / 32));
  uint64_t *chunk = (uint64_t *) &block[index];
  // Modify that chunk in place so that the entire block has the same
  // murmur3 hash but is not a data duplicate.
  m3forward(&chunk[0]);
  m3forward(&chunk[2]);
  chunk[0] ^= 0x0000001000000000UL;
  chunk[1] ^= 0x0000000100000000UL;
  chunk[2] ^= 0x8000000000000000UL;
  m3backward(&chunk[0]);
  m3backward(&chunk[2]);
}

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
      /* fall through */
    case 'M':
    case 'm':
      value *= 1024;
      /* fall through */
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
static bool readBlock(int fd, void *block, off_t blockSize, const char *path)
{
  ssize_t n = read(fd, block, blockSize);
  if (n < 0) {
    err(3, "read failure on %s", path);
  }
  if (n == 0) {
    return true;
  }
  if (n != blockSize) {
    errx(3, "short read on %s", path);
  }
  return false;
}

/**********************************************************************/
static void writeBlock(int fd, void *block, off_t blockSize, const char *path)
{
  ssize_t n = write(fd, block, blockSize);
  if (n < 0) {
    err(3, "write failure on %s", path);
  }
  if (n != blockSize) {
    errx(3, "short write on %s", path);
  }
}

/**********************************************************************/
static void usage(int verbose)
{
  fprintf(stderr,
"Usage:  murmur3collide [--help] [--bs=N] [--count=N] --if=<path> --of=<path>"
"                       [--seek=N] [--skip=N]\n");
  if (verbose) {
    fprintf(stderr,
            "\n"
            "\t--bs=N     sets the blocksize to N (default 4K)\n"
            "\n"
            "\t--count=N  sets the block count to N (default 1)\n"
            "\n"
            "\t--if=Path  sets the pathname of the input file\n"
            "\n"
            "\t--of=Path  sets the pathname of the output file\n"
            "\n"
            "\t--seek=N   skips over N blocks before writing (default 0)\n"
            "\n"
            "\t--skip=N   skips over N blocks before reading (default 0)\n"
            "\n"
            "\t--help     prints this help\n");
  }
  exit(verbose ? 0 : 1);
}

/**********************************************************************/
int main(int argc, char **argv)
{
  bool doFsync = false;
  bool verify = false;
  char *ipath = NULL;
  char *opath = NULL;
  off_t blockSize = 4096;
  int count = 1;
  int seek = 0;
  int skip = 0;

  struct option options[] = {
    {"bs",      required_argument, NULL, 'b'},
    {"count",   required_argument, NULL, 'c'},
    {"fsync",   no_argument,       NULL, 'f'},
    {"help",    no_argument,       NULL, 'h'},
    {"if",      required_argument, NULL, 'i'},
    {"of",      required_argument, NULL, 'o'},
    {"seek",    required_argument, NULL, 's'},
    {"skip",    required_argument, NULL, 't'},
    {"verify",  no_argument,       NULL, 'v'},
    {NULL,      0,                 NULL,  0 },
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
    case 'f': // --fsync
      doFsync = true;
      break;
    case 'h': // --help
      usage(1);
      break;
    case 'i': // --if=<path>
      ipath = optarg;
      break;
    case 'o': // --of=<path>
      opath = optarg;
      break;
    case 's': // --seek=<count>
      seek = numConvert(optarg);
      break;
    case 't': // --skip=<count>
      skip = numConvert(optarg);
      break;
    case 'v': // --verify
      verify = true;
      break;
    default:
      usage(0);
      break;
    }
  }
  if (optind < argc) {
    usage(0);
  }

  // Block size will nearly always be 4K, but do some sanity checking to
  // verify assumptions that the code will be making.
  if (blockSize < 32) {
    errx(4, "blockSize (%ld) is too small", blockSize);
  }
  if (blockSize % 32 != 0) {
    errx(4, "blockSize (%ld) must be a multiple of 32", blockSize);
  }

  int ifd = open(ipath, O_RDONLY);
  if (ifd < 0) {
    err(3, "open failure on %s", ipath);
  }
  int ofd = (verify
             ? open(opath, O_RDONLY)
             : open(opath, O_WRONLY|O_CREAT, 0666));
  if (ofd < 0) {
    err(3, "open failure on %s", opath);
  }

  if (lseek(ifd, skip * blockSize, SEEK_SET) < 0) {
    err(3, "lseek failure on %s", ipath);
  }
  if (lseek(ofd, seek * blockSize, SEEK_SET) < 0) {
    err(3, "lseek failure on %s", opath);
  }

  for (int i = 0; i < count; i++) {
    unsigned char block[blockSize];
    if (readBlock(ifd, block, blockSize, ipath)) {
      break;
    }
    collide(block, blockSize);
    if (verify) {
      unsigned char buffer[blockSize];
      if (readBlock(ofd, buffer, blockSize, opath)) {
        errx(5, "end of file on %s", opath);
      }
      if (memcmp(block, buffer, blockSize) != 0) {
        errx(5, "block %d mismatch", count);
      }
    } else {
      writeBlock(ofd, block, blockSize, opath);
    }
  }

  if (close(ifd) != 0) {
    err(3, "close failure on %s", ipath);
  }
  if (doFsync && (fsync(ofd) != 0)) {
    err(3, "fsync failure on %s", opath);
  }
  if (close(ofd) != 0) {
    err(3, "close failure on %s", opath);
  }

  return 0;
}
