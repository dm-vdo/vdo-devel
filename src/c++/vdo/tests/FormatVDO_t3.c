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
#include "encodings.h"
#include "slab-depot.h"
#include "vdo.h"
#include "vdoConfig.h"

#include "userVDO.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void minimumVDOTest(void)
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
static void minimumVDOTestInKernel(void)
{
  const TestParameters parameters = {
    .indexMemory        = UDS_MEMORY_CONFIG_256MB,
    .journalBlocks      = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
    .slabJournalBlocks  = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
    .slabSize           = 512, // Need more size to fit journal slab default in kernel
    .slabCount          = 1,
    // Geometry block + super block + root count + one slab + recovery journal
    // + slab summary
    .physicalBlocks     = 1 + 1 + 60 + 512 + DEFAULT_VDO_RECOVERY_JOURNAL_SIZE + VDO_SLAB_SUMMARY_BLOCKS,
    .formatInKernel     = true,
  };
  initializeVDOTest(&parameters);

  VDO_ASSERT_SUCCESS(vdo_validate_component_states(&vdo->states,
                                                   vdo->geometry.nonce,
                                                   vdo->device_config->physical_blocks,
                                                   vdo->device_config->logical_blocks));
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Minimum VDO Size Test", minimumVDOTest },
  { "Minimum VDO Size Test (kernel formatting)", minimumVDOTestInKernel },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO format tests (FormatVDO_t3)",
  .initializerWithArguments = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
