/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map.h"
#include "constants.h"

#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

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
  return config;
}

/**********************************************************************/
static void initialize(void)
{
  TestParameters testParameters = {
    .physicalBlocks     = PHYSICAL_BLOCKS,
    .slabSize           = 256,
    .modifier           = zeroLogicalBlocks,
    .synchronousStorage = true,
  };
  initializeVDOTest(&testParameters);
}

/**********************************************************************/
static void testDefaultLogicalBlocks(void)
{
  // Make sure there's enough space for at least one logical block per root so
  // every possible block map page will be populated.
  block_count_t logicalBlocks = getTestConfig().config.logical_blocks;
  block_count_t leafPages = vdo_compute_block_map_page_count(logicalBlocks);
  CU_ASSERT_TRUE(leafPages >= DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  CU_ASSERT_EQUAL(logicalBlocks, populateBlockMapTree());
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Default logical blocks", testDefaultLogicalBlocks },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Default format parameters tests (FormatVDO_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

/**********************************************************************/
CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
