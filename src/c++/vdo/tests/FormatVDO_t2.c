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
#include "vdo.h"

#include "userVDO.h"
#include "ioRequest.h"
#include "vdoConfig.h"
#include "asyncLayer.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

#include "testParameters.h"

enum {
  // Must be large enough to have enough logical space to span all tree roots.
  PHYSICAL_BLOCKS = DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 1024 * 2,
};

/**
 * Implements ConfigurationModifier.
 **/
static TestConfiguration zeroLogicalBlocks(TestConfiguration config)
{
  config.config.logical_blocks = 0;
  config.deviceConfig.logical_blocks = 0;
  return config;
}

/**********************************************************************/
static void testDefaultLogicalBlocks(void)
{
  TestParameters testParameters = {
    .physicalBlocks     = PHYSICAL_BLOCKS,
    .slabSize           = 256,
    .modifier           = zeroLogicalBlocks,
    .synchronousStorage = true,
  };
  initializeTest(&testParameters);
  
  stopVDO();

  // Generate a uuid.
  uuid_t uuid;
  uuid_generate(uuid);

  TestConfiguration configuration = getTestConfig();
  struct index_config *indexConfig = ((configuration.indexConfig.mem == 0)
                                      ? NULL
                                      : &configuration.indexConfig);
  VDO_ASSERT_SUCCESS(formatVDOWithNonce(&configuration.config,
                                        indexConfig,
                                        getSynchronousLayer(),
                                        current_time_us(),
                                        &uuid));
  startAsyncLayer(configuration, true);

  // Make sure there's enough space for at least one logical block per root so
  // every possible block map page will be populated.
  block_count_t logicalBlocks = configuration.config.logical_blocks;
  block_count_t leafPages = vdo_compute_block_map_page_count(logicalBlocks);
  CU_ASSERT_TRUE(leafPages >= DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  for (block_count_t i = 0; i < leafPages; i++) {
    zeroData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, VDO_SUCCESS);
    discardData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, VDO_SUCCESS);
  }
  CU_ASSERT_EQUAL(logicalBlocks, getPhysicalBlocksFree());
}

/**********************************************************************/
static void testDefaultLogicalBlocksInKernel(void)
{
  // These default values are taken from vdoFormat.c
  TestParameters testParameters = {
    .indexMemory        = UDS_MEMORY_CONFIG_256MB,
    .journalBlocks      = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
    .slabJournalBlocks  = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
    .modifier           = zeroLogicalBlocks,
   .formatInKernel     = true,
  };
  
  // In kernel formatting should not accept 0 as the logical size
  initializeTest(&testParameters);
  formatTestVDO();
  startVDOExpectError(vdo_status_to_errno(VDO_BAD_CONFIGURATION));
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Default logical blocks", testDefaultLogicalBlocks },
  { "Default logical blocks (kernel formatting)", testDefaultLogicalBlocksInKernel },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Default format parameters tests (FormatVDO_t2)",
  .initializerWithArguments = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

/**********************************************************************/
CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
