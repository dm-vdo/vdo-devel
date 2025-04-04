/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/random.h>
#include <string.h>

#include "albtest.h"
#include "assertions.h"
#include "vio.h"

/**********************************************************************/
static void isZeroTest(void)
{
  static char testBlock[VDO_BLOCK_SIZE];
  static char dataBlock[VDO_BLOCK_SIZE];

  get_random_bytes(dataBlock, sizeof(dataBlock));
  // The tests below assume leading/trailing bytes are nonzero.
  if (dataBlock[0] == 0) {
    dataBlock[0] = 1;
  }
  if (dataBlock[VDO_BLOCK_SIZE-1] == 0) {
    dataBlock[VDO_BLOCK_SIZE-1] = 1;
  }

  // All zeros
  memset(testBlock, 0, sizeof(testBlock));
  CU_ASSERT_TRUE(vdo_is_vio_data_zero(testBlock));

  // A run of zeros at the beginning
  for (int i = VDO_BLOCK_SIZE - 1; i >= 0; i--) {
    testBlock[i] = dataBlock[i];
    CU_ASSERT_FALSE(vdo_is_vio_data_zero(testBlock));
  }
  // A run of zeros at the end
  for (int i = VDO_BLOCK_SIZE - 1; i > 0; i -= 1) {
    testBlock[i] = 0;
    CU_ASSERT_FALSE(vdo_is_vio_data_zero(testBlock));
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
