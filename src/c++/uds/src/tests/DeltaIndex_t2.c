// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/bits.h>
#include <linux/cache.h>

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"

/*
 * This test ensures the portability of delta indices across platforms.  It is
 * a requirement that any index written on any platform can be used on any
 * other platform.  We do accesses to byte and bit streams exactly as the delta
 * index code does, and test that we get the expected results.
 */

/**
 * Portability of delta indices depends upon an unaligned load acting little
 * endian and compatible with x86.
 **/
static void unalignedTest(void)
{
  enum { MEM_SIZE = sizeof(uint32_t) + L1_CACHE_BYTES};
  u8 memory[MEM_SIZE];
  int i;
  for (i = 0; i < MEM_SIZE; i++) {
    memory[i] = (u8) i;
  }
  for (i = 0; i + sizeof(uint32_t) < MEM_SIZE; i++) {
    uint32_t expect = (memory[i]
                       | (memory[i + 1] << BITS_PER_BYTE)
                       | (memory[i + 2] << (2 * BITS_PER_BYTE))
                       | (memory[i + 3] << (3 * BITS_PER_BYTE)));
    CU_ASSERT_EQUAL(expect, get_unaligned_le32(memory + i));
  }
}

/**
 * Portability of delta indices depends upon ffs always being little
 * endian and compatible with x86.
 **/
static void ffsTest(void)
{
  unsigned int i, j;
  for (i = 1; i < (1 << BITS_PER_BYTE); i++) {
    for (j = 0; j < sizeof(uint32_t); j++) {
      uint32_t data = i << (j * BITS_PER_BYTE);
      int first = ffs(data);
      uint32_t firstBit = 1u << (first - 1);
      CU_ASSERT_EQUAL(firstBit, firstBit & data);
      CU_ASSERT_EQUAL(0, (firstBit - 1) & data);
    }
  }
}

/**********************************************************************/

static const CU_TestInfo bitsTests[] = {
  {"Unaligned", unalignedTest },
  {"Ffs",       ffsTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo bitsSuite = {
  .name  = "DeltaIndex_t2",
  .tests = bitsTests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &bitsSuite;
}
