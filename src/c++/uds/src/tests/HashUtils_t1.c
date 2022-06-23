// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "hash-utils.h"

#include "albtest.h"
#include "assertions.h"

/**********************************************************************/
static void testComputeBits(void)
{
  CU_ASSERT_EQUAL(0, compute_bits(0));
  CU_ASSERT_EQUAL(1, compute_bits(1));
  CU_ASSERT_EQUAL(2, compute_bits(2));

  // Test all the remaining boundaries.
  unsigned int bit;
  for (bit = 2; bit < 32; bit++) {
    unsigned int x = (1 << bit);
    CU_ASSERT_EQUAL(bit + 1, compute_bits(x));
    CU_ASSERT_EQUAL(bit + 1, compute_bits(x + 1));
    CU_ASSERT_EQUAL(bit, compute_bits(x - 1));
  }
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"computeBits", testComputeBits },
  CU_TEST_INFO_NULL
};

static const CU_SuiteInfo suite = {
  .name  = "HashUtils_t1",
  .tests = tests
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
