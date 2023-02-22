/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map.h"
#include "slab-depot.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**
 * Test-specific initialization.
 **/
static void initializeLoadParametersT1(void)
{
  TestParameters parameters = {
    .mappableBlocks = 64,
  };
  initializeVDOTest(&parameters);
}

/**********************************************************************/
static void testNewCacheSize(void)
{
  // Double the cache size.
  struct device_config deviceConfig = getTestConfig().deviceConfig;
  deviceConfig.cache_size *= 2;
  reloadVDO(deviceConfig);

  page_count_t cacheSize = vdo->block_map->zones[0].page_cache.page_count;
  CU_ASSERT_EQUAL(deviceConfig.cache_size, cacheSize);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "new block map cache size", testNewCacheSize },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "LoadParameters_t1",
  .initializerWithArguments = NULL,
  .initializer              = initializeLoadParametersT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
