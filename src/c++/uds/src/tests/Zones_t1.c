// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "funnel-requestqueue.h"
#include "index.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static struct configuration *config;
static struct uds_index     *theIndex;

static struct cond_var callbackCond;
static struct mutex    callbackMutex;
static unsigned int    callbackCount = 0;

/**
 * A test callback that simply counts callbacks.
 **/
static void testCallback(struct uds_request *request)
{
  uds_lock_mutex(&callbackMutex);
  callbackCount++;
  uds_signal_cond(&callbackCond);
  uds_unlock_mutex(&callbackMutex);
  freeRequest(request);
}

/**
 * The suite initialization function.
 **/
static void zoneInitializeSuite(const char *indexName)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  // Creating an index also creates the zone queues.
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_CREATE, NULL, &testCallback, &theIndex));
  UDS_ASSERT_SUCCESS(uds_init_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_init_mutex(&callbackMutex));
}

/**
 * The suite cleanup function.
 **/
static void zoneFinishSuite(void)
{
  uds_free_index(theIndex);
  uds_free_configuration(config);
  UDS_ASSERT_SUCCESS(uds_destroy_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_destroy_mutex(&callbackMutex));
}

/**
 * Wait for the expected number of callbacks.
 **/
static void waitForCallbacks(unsigned int expectedCount)
{
  uds_lock_mutex(&callbackMutex);
  while (callbackCount < expectedCount) {
    uds_wait_cond(&callbackCond, &callbackMutex);
  }
  uds_unlock_mutex(&callbackMutex);
  callbackCount = 0;
}

/**********************************************************************/
static void addBlocksToZone(unsigned int zone, unsigned int count)
{
  struct uds_record_data metadata;
  createRandomMetadata(&metadata);
  unsigned int i;
  for (i = 0; i < count; i++) {
    struct uds_request *request;
    UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct uds_request, "request",
                                    &request));
    request->new_metadata = metadata;
    request->index        = theIndex;
    request->type         = UDS_POST;
    request->unbatched    = true;
    createRandomBlockNameInZone(theIndex, zone, &request->record_name);
    uds_enqueue_request(request, STAGE_INDEX);
  }
}

/**
 * Make sure the chapter close messages were processed by sending
 * a block down each queue after it.  We can't track the control
 * message, but we can track the subsequent request(s).
 **/
static void flushZoneQueues(unsigned int zoneCount)
{
  unsigned int i;
  for (i = 0; i < zoneCount; i++) {
    addBlocksToZone(i, 1);
  }
  waitForCallbacks(zoneCount);
}

/**
 * Test the closing chapter message by intentionally running most
 * chunks into one zone and verifying that the other zones stay in
 * sync.
 **/
static void laggingZonesTest(void)
{
  unsigned int zoneCount = config->zone_count;
  struct geometry *geometry = theIndex->volume->geometry;
  unsigned int recordsPerChapter = geometry->records_per_chapter;
  unsigned int recordsPerZone = theIndex->zones[0]->open_chapter->capacity;

  // First, test closing a zone chapter when other zones are quiescent.
  // Add blocks in one zone to mostly fill that zone.
  uint64_t newestChapter = 0;
  addBlocksToZone(0, recordsPerZone - 1);
  waitForCallbacks(recordsPerZone - 1);

  // Assert that no zone chapter has closed.
  unsigned int z;
  for (z = 0; z < zoneCount; z++) {
    struct index_zone *zone = theIndex->zones[z];
    CU_ASSERT_EQUAL(zone->newest_virtual_chapter, newestChapter);
  }
  CU_ASSERT_EQUAL(newestChapter, theIndex->newest_virtual_chapter);

  // Add one more block and assert all zone chapters have closed.
  addBlocksToZone(0, 1);
  waitForCallbacks(1);
  flushZoneQueues(zoneCount);
  newestChapter = 1;
  for (z = 0; z < zoneCount; z++) {
    struct index_zone *zone = theIndex->zones[z];
    CU_ASSERT_EQUAL(zone->newest_virtual_chapter, newestChapter);
  }
  uds_wait_for_idle_index(theIndex);
  CU_ASSERT_EQUAL(newestChapter, theIndex->newest_virtual_chapter);

  // Second, test closing a zone chapter when other zones have requests.
  // Add blocks in one zone to mostly fill that zone again.
  addBlocksToZone(0, (3 * recordsPerZone / 4));
  waitForCallbacks(3* recordsPerZone / 4);
  flushZoneQueues(zoneCount);
  for (z = 0; z < zoneCount; z++) {
    struct index_zone *zone = theIndex->zones[z];
    CU_ASSERT_EQUAL(zone->newest_virtual_chapter, newestChapter);
  }
  CU_ASSERT_EQUAL(newestChapter, theIndex->newest_virtual_chapter);

  // Add a half chapter worth of blocks across all zones.
  struct uds_record_name name;
  struct uds_record_data metadata;
  createRandomMetadata(&metadata);
  unsigned int i;
  for (i = 0; i < (recordsPerChapter / 2); i++) {
    createRandomBlockName(&name);
    struct uds_request *request;
    UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct uds_request, "request",
                                    &request));
    request->record_name  = name;
    request->new_metadata = metadata;
    request->index        = theIndex;
    request->type         = UDS_POST;
    request->unbatched    = true;
    uds_enqueue_request(request, STAGE_INDEX);
  }

  waitForCallbacks(recordsPerChapter / 2);
  flushZoneQueues(zoneCount);
  newestChapter = 2;
  for (z = 0; z < zoneCount; z++) {
    struct index_zone *zone = theIndex->zones[z];
    CU_ASSERT_EQUAL(zone->newest_virtual_chapter, newestChapter);
  }
  uds_wait_for_idle_index(theIndex);
  CU_ASSERT_EQUAL(newestChapter, theIndex->newest_virtual_chapter);
}

static const CU_TestInfo zoneTests[] = {
  { "Lagging Zones", laggingZonesTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Zones_t1",
  .initializerWithIndexName = zoneInitializeSuite,
  .cleaner                  = zoneFinishSuite,
  .tests                    = zoneTests,
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
