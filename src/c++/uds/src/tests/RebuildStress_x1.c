// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * RebuildStress_x1 (formerly Index_p4) tests that we can recover after an
 * unclean shutdown of the Albireo index.
 *
 * Starting with an empty index, it enters a loop where it adds a random number
 * of chunks to the index (adding up to 1/4 of the index) and then exits
 * without doing a clean shutdown.  The first time through the loop loads an
 * empty index.  Each other trip through the loop loads the unclean index left
 * by the previous trip.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "dory.h"
#include "indexer.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"
#include "thread-utils.h"

static struct block_device *testDevice;

// This semaphore limits the number of simultaneous requests that will be sent
// to the index.
static struct semaphore semaphore;

/**********************************************************************/
static void finishChunk(struct uds_request *udsRequest)
{
  uds_free(udsRequest);
  uds_release_semaphore(&semaphore);
}

/**********************************************************************/
static void reportIndexSize(struct uds_index_session *indexSession,
                            struct uds_index_stats   *indexStats)
{
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, indexStats));
  albPrint("%llu entries indexed with %llu collisions",
           (unsigned long long) indexStats->entries_indexed,
           (unsigned long long) indexStats->collisions);
}

/**********************************************************************/
static void fullRebuildTest(void)
{
  unsigned long counter = 0;
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&semaphore, 2000));

  // Create a new index.
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };
  randomizeUdsNonce(&params);
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  unsigned int  numBlocksPerChapter = getBlocksPerChapter(indexSession);
  unsigned long numBlocksPerIndex   = getBlocksPerIndex(indexSession);

  ktime_t startLoop = current_time_ns(CLOCK_MONOTONIC);
  int loopCount = 0;
  do {
    albPrint("===== RebuildStress_x1 ===== Pass %d =====", ++loopCount);
    unsigned int numBlocks
      = (1 << 20) + random() % ((numBlocksPerIndex / 4) - (1 << 20) + 1);
    albPrint("Add %d chunks to the index", numBlocks);
    albFlush();
    unsigned int i;
    for (i = 0; i < numBlocks; i++) {
      struct uds_request *udsRequest;
      UDS_ASSERT_SUCCESS(uds_allocate(1, struct uds_request, __func__,
                                      &udsRequest));
      uds_acquire_semaphore(&semaphore);
      udsRequest->record_name = hash_record_name(&counter, sizeof(counter));
      udsRequest->callback    = finishChunk;
      udsRequest->session     = indexSession;
      udsRequest->type        = UDS_POST;
      UDS_ASSERT_SUCCESS(uds_launch_request(udsRequest));
      counter++;
    }
    // Report the index size
    struct uds_index_stats indexStats;
    reportIndexSize(indexSession, &indexStats);
    uint64_t entriesIndexed = indexStats.entries_indexed;
    // Turn off writing, and do a dirty closing of the index.
    set_dory_forgetful(true);
    UDS_ASSERT_ERROR(-EROFS, uds_close_index(indexSession));
    set_dory_forgetful(false);
    // Make sure the index will not load.
    UDS_ASSERT_ERROR2(-ENOENT, -EEXIST,
                      uds_open_index(UDS_NO_REBUILD, &params, indexSession));
    albFlush();
    // Rebuild the index.
    ktime_t startRebuild = current_time_ns(CLOCK_MONOTONIC);
    UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));
    ktime_t rebuildTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC),
                                    startRebuild);
    char *timeString;
    UDS_ASSERT_SUCCESS(rel_time_to_string(&timeString, rebuildTime));
    albPrint("Index rebuilt in %s", timeString);
    uds_free(timeString);
    reportIndexSize(indexSession, &indexStats);
    // Report lost entries
    if (entriesIndexed > indexStats.entries_indexed) {
      uint64_t lostEntries = entriesIndexed - indexStats.entries_indexed;
      albPrint("Lost %llu entries", (unsigned long long) lostEntries);
    }
    // Expect that rebuilding the index lost no more than 5 chapters of
    // entries.  We must be careful to not underflow an unsigned value.
    CU_ASSERT(indexStats.entries_indexed + 5 * numBlocksPerChapter
              >= entriesIndexed);
  } while (ktime_to_seconds(ktime_sub(current_time_ns(CLOCK_MONOTONIC),
                                      startLoop))
           < 3600);

  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_semaphore(&semaphore));
}

/**********************************************************************/
static void initializerWithBlockDevice(struct block_device *bdev)
{
  testDevice = bdev;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Full Rebuild", fullRebuildTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "RebuildStress_x1",
  .initializerWithBlockDevice = initializerWithBlockDevice,
  .tests                      = tests,
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
