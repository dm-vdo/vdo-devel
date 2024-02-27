// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * Test memory allocation failures that happen during the rebuilding of a local
 * index.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "dory.h"
#include "indexer.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static struct block_device *testDevice;

enum { NUM_CHUNKS = 1000 };

/**********************************************************************/
static size_t getBytesUsed(void)
{
  uint64_t bytesUsed, peakBytesUsed;
  vdo_get_memory_stats(&bytesUsed, &peakBytesUsed);
  return bytesUsed;
}

/**********************************************************************/
static void postChunks(struct uds_index_session *indexSession, int count)
{
  static int base = 0;
  long index;
  for (index = base; index < base + count; index++) {
    struct uds_record_name chunkName = hash_record_name(&index, sizeof(index));
    oldPostBlockName(indexSession, NULL, (struct uds_record_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
}

/**********************************************************************/
static void besmirchIndex(struct uds_index_session *indexSession,
                          struct uds_parameters *params)
{
  // Open the cleanly saved index.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, indexSession));
  int numChaptersWritten = atomic_read_acquire(&chapters_written);
  // Write one chapter of chunks.
  unsigned int numBlocksPerChapter = getBlocksPerChapter(indexSession);
  postChunks(indexSession, numBlocksPerChapter + 1000);
  // Wait for the chapter write to complete.
  while (numChaptersWritten == atomic_read_acquire(&chapters_written)) {
    sleep_for(ms_to_ktime(100));
  }
  // Turn off writing, and do a dirty closing of the index.
  set_dory_forgetful(true);
  UDS_ASSERT_ERROR(-EROFS, uds_close_index(indexSession));
  set_dory_forgetful(false);
  /*
   * Now we have written a new chapter to the volume.  We have written neither
   * the volume index nor the index page map, and we have deleted the open
   * chapter.
   */
}

/**********************************************************************/
static void rebuildTest(void)
{
  initializeOldInterfaces(2000);

  // Create a new index and write the base set of 1000 chunks to the index.
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };
  randomizeUdsNonce(&params);
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  postChunks(indexSession, NUM_CHUNKS);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  besmirchIndex(indexSession, &params);

  // Test that failing to rebuild an index does not leak memory.
  unsigned int pass = 1;
  bool loop;
  for (loop = true; loop; pass++) {
    albPrint("Loading Pass %u", pass);
    // Record the number of bytes that have been allocated.
    UDS_ASSERT_SUCCESS(track_uds_memory_allocations(true));
    size_t allocationOverhead = getBytesUsed();
    schedule_uds_memory_allocation_failure(pass);
    int result = uds_open_index(UDS_LOAD, &params, indexSession);
    loop = !uds_allocation_failure_scheduled();
    cancel_uds_memory_allocation_failure();
    if (result == UDS_SUCCESS) {
      UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
    } else {
      UDS_ASSERT_ERROR2(-ENOMEM, -EEXIST, result);
    }
    if (allocationOverhead < getBytesUsed()) {
      log_uds_memory_allocations();
    }
    CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());
    // If the rebuild has succeeded in spite of a memory allocation error,
    // we need make the index require another rebuild.
    if (loop && (result == UDS_SUCCESS)) {
      besmirchIndex(indexSession, &params);
    }
  }

  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(false));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithBlockDevice(struct block_device *bdev)
{
  testDevice = bdev;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Allocation during rebuild", rebuildTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "AllocFail_x4",
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
