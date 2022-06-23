/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"
#include "assertions.h"
#include "data-vio.h"
#include "random.h"
#include <string.h>

/**********************************************************************/
static void isZeroTest(void)
{
  static char testBlock[VDO_BLOCK_SIZE];
  static char dataBlock[VDO_BLOCK_SIZE];

  fill_randomly(dataBlock, sizeof(dataBlock));

  // All zeros
  memset(testBlock, 0, sizeof(testBlock));
  CU_ASSERT_TRUE(is_zero_block(testBlock));

  // A run of zeros at the beginning
  for (int i = VDO_BLOCK_SIZE - 1; i >= 0; i--) {
    testBlock[i] = dataBlock[i];
    CU_ASSERT_FALSE(is_zero_block(testBlock));
  }
  // A run of zeros at the end
  for (int i = VDO_BLOCK_SIZE - 1; i > 0; i -= 1) {
    testBlock[i] = 0;
    CU_ASSERT_FALSE(is_zero_block(testBlock));
  }
}

/**********************************************************************/
static CU_TestInfo theTestInfo[] = {
  { "zero block", isZeroTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo theSuiteInfo = {
  .name                     = "Test is_zero_block (IsZero_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = theTestInfo
};

CU_SuiteInfo *initializeModule(void)
{
  return &theSuiteInfo;
}
