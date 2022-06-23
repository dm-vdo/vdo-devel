/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map-tree.h"
#include "num-utils.h"
#include "vdoAsserts.h"

/**********************************************************************/
static void testIsPowerOfTwo(void)
{
  // Test the early, adjacent cases.
  CU_ASSERT_FALSE(is_power_of_2(0));
  CU_ASSERT_TRUE(is_power_of_2(1));
  CU_ASSERT_TRUE(is_power_of_2(2));

  // Test all the boundary cases.
  for (uint64_t i = 4; i != 0; i <<= 1) {
    CU_ASSERT_FALSE(is_power_of_2(i - 1));
    CU_ASSERT_TRUE(is_power_of_2(1));
    CU_ASSERT_FALSE(is_power_of_2(i + 1));
  }
}

/**********************************************************************/
static void testILog2(void)
{
  // Test the early, adjacent cases.
  CU_ASSERT_EQUAL(0, ilog2(1));
  CU_ASSERT_EQUAL(1, ilog2(2));

  // Test all the boundary cases.
  for (unsigned int shift = 1; shift < 64; shift++) {
    uint64_t powerOfTwo = (1ULL << shift);
    CU_ASSERT_EQUAL(shift, ilog2(powerOfTwo));
    CU_ASSERT_EQUAL(shift, ilog2(powerOfTwo + 1));
    CU_ASSERT_EQUAL(shift, ilog2(powerOfTwo - 1 + powerOfTwo));
  }
}

/**
 * This is actually testing a function in block-map-tree.c...
 **/
static void testInCyclicRange(void)
{
  CU_ASSERT_FALSE(in_cyclic_range(16, 0, 48, 64));
  CU_ASSERT_FALSE(in_cyclic_range(16, 8, 48, 64));
  CU_ASSERT_TRUE(in_cyclic_range(16, 16, 48, 64));
  CU_ASSERT_TRUE(in_cyclic_range(16, 30, 48, 64));
  CU_ASSERT_TRUE(in_cyclic_range(16, 48, 48, 64));
  CU_ASSERT_FALSE(in_cyclic_range(16, 60, 48, 64));
  CU_ASSERT_FALSE(in_cyclic_range(16, 63, 48, 64));

  CU_ASSERT_TRUE(in_cyclic_range(48, 0, 16, 64));
  CU_ASSERT_TRUE(in_cyclic_range(48, 8, 16, 64));
  CU_ASSERT_TRUE(in_cyclic_range(48, 16, 16, 64));
  CU_ASSERT_FALSE(in_cyclic_range(48, 30, 16, 64));
  CU_ASSERT_TRUE(in_cyclic_range(48, 48, 16, 64));
  CU_ASSERT_TRUE(in_cyclic_range(48, 60, 16, 64));
  CU_ASSERT_TRUE(in_cyclic_range(48, 63, 16, 64));

  CU_ASSERT_FALSE(in_cyclic_range(20, 10, 20, 64));
  CU_ASSERT_TRUE(in_cyclic_range(20, 20, 20, 64));
  CU_ASSERT_FALSE(in_cyclic_range(20, 40, 20, 64));

  CU_ASSERT_TRUE(in_cyclic_range(20, 10, 19, 64));
  CU_ASSERT_TRUE(in_cyclic_range(20, 19, 19, 64));
  CU_ASSERT_TRUE(in_cyclic_range(20, 20, 19, 64));
  CU_ASSERT_TRUE(in_cyclic_range(20, 40, 19, 64));
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "is_power_of_2",     testIsPowerOfTwo      },
  { "ilog2",		 testILog2	       },
  { "in_cyclic_range",   testInCyclicRange     },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name  = "Simple numUtils tests (NumUtils_t1)",
  .tests = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
