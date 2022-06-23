// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 *
 * Test of starting and stopping an index.
 */

/**
 * Index_p1 measures the time to start and stop the index.
 *
 * While it also measures the time to fill the index, the performance of
 * indexing chunk names is not the focus of this test.  If this is what
 * you are looking for, you should be looking at PostBlockName_p1.
 **/

#include <linux/atomic.h>

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "memory-alloc.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"

static const char *indexName;

/**********************************************************************/
static void reportDuration(const char *label, ktime_t start, ktime_t stop)
{
  ktime_t duration = ktime_sub(stop, start);
  char *timeString;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&timeString, duration, 0));
  albPrint("%s in %s", label, timeString);
  UDS_FREE(timeString);
  albFlush();
}

/**********************************************************************/
static void testRunner(struct uds_parameters *params)
{
  ktime_t startTime, stopTime;
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  albPrint(" ");

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, params, indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Index created", startTime, stopTime);

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Empty index saved", startTime, stopTime);

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Empty index loaded", startTime, stopTime);

  // Fill the index, and then add chunks to fill 16 more chapters.  This will
  // add more entries to the volume index that are due to be LRUed away.
  uint64_t numBlocksToWrite = getBlocksPerIndex(indexSession);
  numBlocksToWrite += 16 * getBlocksPerChapter(indexSession);

  initializeOldInterfaces(2000);
  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  uint64_t counter;
  for (counter = 0; counter < numBlocksToWrite; counter++) {
    struct uds_chunk_name chunkName
      = murmurHashChunkName(&counter, sizeof(counter), 0);
    oldPostBlockName(indexSession, NULL, (struct uds_chunk_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Index filled", startTime, stopTime);
  uninitializeOldInterfaces();

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Full index saved", startTime, stopTime);

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Full index loaded", startTime, stopTime);

  WRITE_ONCE(startTime, current_time_ns(CLOCK_MONOTONIC));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  WRITE_ONCE(stopTime, current_time_ns(CLOCK_MONOTONIC));
  reportDuration("Full index saved again", startTime, stopTime);
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
}

/**********************************************************************/
static void denseTest(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .name = indexName,
  };
  randomizeUdsNonce(&params);
  testRunner(&params);
}

/**********************************************************************/
static void sparseTest(void)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
    .sparse = true,
  };
  randomizeUdsNonce(&params);
  testRunner(&params);
}

/**********************************************************************/
static void initializerWithIndexName(const char *in)
{
  indexName = in;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "dense",  denseTest },
  { "sparse", sparseTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Index_p1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
