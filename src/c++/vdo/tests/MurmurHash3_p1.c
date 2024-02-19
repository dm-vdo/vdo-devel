/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * Performance testing of MurmurHash3 calculation.
 *
 * $Id$
 */

#include "assertions.h"
#include "indexer.h"
#include "murmurhash3.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/time.h>

enum {
  // Should be larger than CPU cache size.
  TESTSIZE = 40 * 1024 * 1024,
  PREFETCH_AVOIDANCE_GAP = 2048
};

static unsigned char buffer[TESTSIZE] __attribute__ ((__aligned__(64)));

static uint64_t cpuTime(void)
{
  /* user cpu time */
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) < 0) {
    perror("getrusage");
    exit(1);
  }
  return ((uint64_t) ru.ru_utime.tv_sec * 1000000) + ru.ru_utime.tv_usec;
}

static void doMurmurHash(const void *buffer, size_t length)
{
  struct uds_record_name chunkName;
  murmurhash3_128(buffer, length, 0x62ea60be, &chunkName);
}

static void test(size_t        startingOffset,
                 size_t        length,
                 unsigned int  iterations)
{
  unsigned int i;
  /*
   * Run the hash function once before timing it just to make sure
   * code is cached.
   */
  doMurmurHash(buffer, length);
  uint64_t startTime = cpuTime();
  size_t stride = (length + 63 + PREFETCH_AVOIDANCE_GAP) & ~(size_t) 63;
  size_t offset = startingOffset;
  for (i = 0; i < iterations; i++) {
    /*
     * Keep moving around to reduce CPU cache benefits that probably
     * don't match real-world well.  (E.g., one thread creates a
     * buffer and enqueues it for another thread, which may well be on
     * another CPU, to eventually hash, with possibly lots of other
     * memory accesses in the meantime.)  On the other hand, this
     * could exaggerate whatever benefit we might get from
     * prefetching.
     */
    CU_ASSERT_TRUE(offset + length <= sizeof(buffer));
    doMurmurHash(buffer + offset, length);
    offset += stride;
    if ((offset + length) > sizeof(buffer)) {
      offset = startingOffset;
    }
  }
  uint64_t endTime = cpuTime();
  uint64_t duration = endTime - startTime;
  // There are in usec per whatever.
  double perHash = (double) duration / iterations;
  double perByte = perHash / length;
  printf("%8u hashes of %8zuB: %5.2fs (%.3fus/hash, %5.3fns/B, %5.1fMB/s)\n",
         iterations, length, duration * 1.0e-6, perHash, 1000 * perByte,
         (1.0e6 / (1024 * 1024)) / perByte);
}

int main(void)
{
  unsigned int baseIterationCount = 200;
  // Initialize buffer with garbage.
  unsigned int i;
  for (i = 0; i < sizeof(buffer); i++) {
    buffer[i] = random() & 0xff;
  }

  unsigned int bigIterations = baseIterationCount;
  unsigned int smallIterations = baseIterationCount * 100000;
  /*
   * These sizes are probably the most important for us, so the
   * iteration count scaling here causes us to spend a bit more time
   * on these for more accurate(?) numbers.
   */
  unsigned int medium4KIterations = baseIterationCount * 20000;
  unsigned int medium64KIterations = baseIterationCount * 2000;

  printf("Big, aligned:\n");
  test(0, sizeof(buffer), bigIterations);
  printf("Big, unaligned:\n");
  test(3, sizeof(buffer)-3, bigIterations);

  printf("Medium, aligned:\n");
  test(0, 4096, medium4KIterations);
  test(0, 65536, medium64KIterations);
  printf("Medium, unaligned:\n");
  test(5, 4096, medium4KIterations);
  test(5, 65536, medium64KIterations);

  printf("Small, aligned:\n");
  test(0, 256, smallIterations);
  printf("Small, aligned, short of BLOCKSIZE at end:\n");
  test(0, 256-10, smallIterations);
  printf("Small, unaligned:\n");
  test(3, 256, smallIterations);
  return 0;
}
