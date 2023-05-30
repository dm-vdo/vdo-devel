// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "request-queue.h"
#include "testPrototypes.h"

static struct configuration  *config;
static struct uds_index      *theIndex;
static struct mutex           callbackMutex;
static struct cond_var        callbackCond;
static unsigned int           callbackCount = 0;
static enum uds_index_region  lastLocation;

/**********************************************************************/
static void incrementCallbackCount(void)
{
  uds_lock_mutex(&callbackMutex);
  callbackCount++;
  uds_signal_cond(&callbackCond);
  uds_unlock_mutex(&callbackMutex);
}

/**
 * Update the outstanding record count
 * and store the location of the last request.
 **/
static void testCallback(struct uds_request *request)
{
  uds_lock_mutex(&callbackMutex);
  callbackCount--;
  lastLocation = request->location;
  uds_signal_cond(&callbackCond);
  uds_unlock_mutex(&callbackMutex);
}

/**
 * Wait for outstanding callbacks.
 **/
static void waitForCallbacks(void)
{
  uds_lock_mutex(&callbackMutex);
  while (callbackCount > 0) {
    uds_wait_cond(&callbackCond, &callbackMutex);
  }
  uds_unlock_mutex(&callbackMutex);
}

/**********************************************************************/
static void assertLastLocation(enum uds_index_region expectedLocation)
{
  uds_lock_mutex(&callbackMutex);
  CU_ASSERT_EQUAL(expectedLocation, lastLocation);
  uds_unlock_mutex(&callbackMutex);
}

/**********************************************************************/
static void cleanupIndex(void)
{
  uds_free_index(theIndex);
  theIndex = NULL;
}

/**
 * The suite initialization function.
 **/
static void init(const char *indexName)
{
  UDS_ASSERT_SUCCESS(uds_init_mutex(&callbackMutex));
  UDS_ASSERT_SUCCESS(uds_init_cond(&callbackCond));

  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, 0, 0, 4);

  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_CREATE, NULL, &testCallback, &theIndex));
}

/**
 * The suite cleanup function.
 **/
static void deinit(void)
{
  cleanupIndex();
  uds_free_configuration(config);
  UDS_ASSERT_SUCCESS(uds_destroy_mutex(&callbackMutex));
  UDS_ASSERT_SUCCESS(uds_destroy_cond(&callbackCond));
}

/**********************************************************************/
static void dispatchRequest(struct uds_request           *request,
                            enum uds_index_region         expectedLocation,
                            const struct uds_record_data *expectedMetaData)
{
  request->index = theIndex;
  incrementCallbackCount();
  request->unbatched = true;
  uds_enqueue_request(request, STAGE_TRIAGE);
  waitForCallbacks();
  UDS_ASSERT_SUCCESS(request->status);
  assertLastLocation(expectedLocation);
  if (request->found && (expectedMetaData != NULL)) {
    UDS_ASSERT_BLOCKDATA_EQUAL(expectedMetaData, &request->old_metadata);
  }
}

/**********************************************************************/
static void dispatchNonWaitingRequest(struct uds_request *request)
{
  request->index = theIndex;
  request->unbatched = true;
  uds_enqueue_request(request, STAGE_TRIAGE);
}

/**********************************************************************/
static void fillOpenChapter(uint64_t chapterNumber, unsigned int numAdded)
{
  if (theIndex->zone_count == 1) {
    CU_ASSERT_EQUAL(numAdded, theIndex->zones[0]->open_chapter->size);
  }

  static unsigned int zone = 0;
  for (;
       numAdded < theIndex->volume->geometry->records_per_chapter;
       ++numAdded)
  {
    struct uds_request request = { .type = UDS_POST };
    createRandomBlockNameInZone(theIndex, zone, &request.record_name);
    createRandomMetadata(&request.new_metadata);
    dispatchRequest(&request, UDS_LOCATION_UNAVAILABLE, NULL);
    zone = (zone + 1) % theIndex->zone_count;
  }

  uds_wait_for_idle_index(theIndex);
  CU_ASSERT_EQUAL(chapterNumber + 1, theIndex->newest_virtual_chapter);
}

/** Tests **/

/**********************************************************************/
static void readPageThread(void *arg)
{
  struct uds_request *request = (struct uds_request *) arg;
  dispatchNonWaitingRequest(request);
}

/**********************************************************************/
static void testInvalidateChapter(void)
{
  struct uds_request *request, *request2;
  struct volume *volume = theIndex->volume;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct uds_request, __func__, &request));
  request->type = UDS_POST;
  createRandomBlockNameInZone(theIndex, 0, &request->record_name);
  createRandomMetadata(&request->new_metadata);
  dispatchRequest(request, UDS_LOCATION_UNAVAILABLE, NULL);

  fillOpenChapter(0, 1);

  unsigned int i;
  for (i = 1; i < config->geometry->chapters_per_volume - 1; i++) {
    fillOpenChapter(i, 0);
  }

  // Reset the location so we can reuse this request.
  request->location = UDS_LOCATION_UNKNOWN;

  // Stop the read queues from processing entries.
  volume->read_threads_stopped = true;

  struct thread *thread;
  int result = uds_create_thread(readPageThread, request, "readpage", &thread);
  UDS_ASSERT_SUCCESS(result);

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct uds_request, __func__, &request2));
  request2->type = UDS_POST;
  createRandomBlockNameInZone(theIndex, 0, &request2->record_name);
  createRandomMetadata(&request2->new_metadata);
  dispatchRequest(request2, UDS_LOCATION_UNAVAILABLE, NULL);

  fillOpenChapter(config->geometry->chapters_per_volume - 1, 1);

  // Wake the read queues.
  volume->read_threads_stopped = false;

  incrementCallbackCount();
  uds_signal_cond(&volume->read_threads_cond);
  waitForCallbacks();
  assertLastLocation(UDS_LOCATION_UNAVAILABLE);

  // Add some more stuff to make sure the library hasn't been disabled.
  fillOpenChapter(config->geometry->chapters_per_volume, 1);
  UDS_ASSERT_SUCCESS(uds_join_threads(thread));

  UDS_FREE(request);
  UDS_FREE(request2);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"InvalidateChapter",  testInvalidateChapter},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Volume_n5",
  .initializerWithIndexName = init,
  .cleaner                  = deinit,
  .tests                    = tests,
};

// ===========================================================================
// Alternate entry point required by the module loader. Return a pointer to
// the array of const CU_SuiteInfo structures.
// ===========================================================================

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
