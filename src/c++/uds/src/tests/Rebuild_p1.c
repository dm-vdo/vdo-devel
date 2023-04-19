// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 *
 * Rebuild_p1 measures the rebuild performance of a UDS index.
 */

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "oldInterfaces.h"
#include "resourceUsage.h"
#include "testPrototypes.h"

static const char *indexName;

/**********************************************************************/
static void runTest(bool sparse)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
    .sparse = true,
  };

  // Create and fill the index (using UDS interfaces).
  initializeOldInterfaces(1000);
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  unsigned long numRecords = getBlocksPerIndex(indexSession);
  unsigned long i;
  for (i = 0; i < numRecords; i++) {
    struct uds_record_name chunkName = hash_record_name(&i, sizeof(i));
    oldPostBlockName(indexSession, NULL, (struct uds_record_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  uninitializeOldInterfaces();

  // Discard the index state so that we need to do a full rebuild (using index
  // interfaces).
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  config->zone_count = 1;
  struct uds_index *index;
  UDS_ASSERT_SUCCESS(make_index(config, UDS_NO_REBUILD, NULL, NULL, &index));
  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  free_index(index);
  uds_free_configuration(config);

  // Rebuild the volume index.  This is where we do the performance timing.
  ThreadStatistics *preThreadStats = getThreadStatistics();
  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));
  ktime_t loadElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
  ThreadStatistics *postThreadStats = getThreadStatistics();
  char *elapsed;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&elapsed, loadElapsed));
  albPrint("Rebuild %s index in %s", sparse ? "sparse" : "dense", elapsed);
  UDS_FREE(elapsed);
  printThreadStatistics(preThreadStats, postThreadStats);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));

  freeThreadStatistics(postThreadStats);
  freeThreadStatistics(preThreadStats);
}

/**********************************************************************/
static void testDense(void)
{
  runTest(false);
}

/**********************************************************************/
static void testSparse(void)
{
  runTest(true);
}

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  indexName = name;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Dense",  testDense  },
  {"Sparse", testSparse },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Rebuild_p1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
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
