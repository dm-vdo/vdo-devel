/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "syscalls.h"

#include "constants.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vdoConfig.h"
#include "vdo-layout.h"

#include "userVDO.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

static void initializeMinTest(void)
{
  const TestParameters parameters = {
    .journalBlocks  = 32,
    .slabCount      = 1,
    .slabSize       = 8,
    // Geometry block + super block + root count + one slab + recovery journal
    // + slab summary
    .physicalBlocks = 1 + 1 + 60 + 8 + 32 + 64,
  };
  initializeBasicTest(&parameters);
}

/**********************************************************************/
static void minimumVDOTest(void)
{
  TestConfiguration config = getTestConfig();
  block_count_t minBlocks;
  VDO_ASSERT_SUCCESS(calculateMinimumVDOFromConfig(&config.config,
                                                   &config.indexConfig,
                                                   &minBlocks));
  CU_ASSERT_EQUAL(minBlocks, config.config.physical_blocks);
  formatTestVDO();
  UserVDO *vdo;
  VDO_ASSERT_SUCCESS(loadVDO(getSynchronousLayer(), true, &vdo));
  freeUserVDO(&vdo);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Minimum VDO Size Test", minimumVDOTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO format tests (FormatVDO_t3)",
  .initializerWithArguments = NULL,
  .initializer              = initializeMinTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
