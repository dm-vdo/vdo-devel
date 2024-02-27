// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 *
 * Test of steady state indexing performance.
 */

/**
 * PostBlockName_x1 measures the average throughput of udsPostBlockName()
 * at various levels of dedupe.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"

static struct uds_index_session *indexSession;

/**********************************************************************/
static void pbnPerfTest(void)
{
  bool sparseFlag = isIndexSparse(indexSession);
  initializeOldInterfaces(2000);

  // Fill the index
  unsigned long newCounter = 0;
  unsigned long numBlocksPerIndex = getBlocksPerIndex(indexSession);
  unsigned long n;
  for (n = 0; n < numBlocksPerIndex; n++) {
    struct uds_record_name chunkName
      = hash_record_name(&newCounter, sizeof(newCounter));
    newCounter += 1;
    oldPostBlockName(indexSession, NULL, (struct uds_record_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));

  // Outer loop to try various levels of dedupe
  enum { NUM_LEVELS = 16 };
  unsigned long numBlocksPerLevel = numBlocksPerIndex / NUM_LEVELS;
  unsigned int level;
  for (level = 0; level <= NUM_LEVELS; level++) {
    // Inner loop at the specified dedupe level
    unsigned long dupCounter = (level + 2) * numBlocksPerLevel;
    unsigned long startNewCounter = newCounter;
    unsigned long startDupCounter = dupCounter;
    struct uds_index_stats beforeStats;
    UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &beforeStats));
    ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
    unsigned long i;
    for (i = 0; i < numBlocksPerLevel; i++) {
      unsigned long *counter
        = (i % NUM_LEVELS < level) ? &dupCounter : &newCounter;
      struct uds_record_name chunkName
        = hash_record_name(counter, sizeof(*counter));
      *counter += 1;
      oldPostBlockName(indexSession, NULL,
                       (struct uds_record_data *) &chunkName,
                       &chunkName, cbStatus);
    }
    UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
    ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
    // Check for correct dedupe found
    struct uds_index_stats afterStats;
    UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &afterStats));
    CU_ASSERT_EQUAL(numBlocksPerLevel,
                    afterStats.requests - beforeStats.requests);
    if (!sparseFlag) {
      CU_ASSERT_EQUAL(dupCounter - startDupCounter,
                      afterStats.posts_found - beforeStats.posts_found);
      CU_ASSERT_EQUAL(newCounter - startNewCounter,
                      afterStats.posts_not_found
                        - beforeStats.posts_not_found);
    }
    // Report the dedupe performance
    char *perBlock;
    UDS_ASSERT_SUCCESS(rel_time_to_string(&perBlock,
                                          elapsed / numBlocksPerLevel));
    albPrint("%3u%% dedupe, %s per iteration", 100 * level / NUM_LEVELS,
             perBlock);
    vdo_free(perBlock);
  }

  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "post block name performance", pbnPerfTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "PostBlockName_x1",
  .initializerWithSession   = initializerWithSession,
  .oneIndexConfiguredByArgv = true,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
