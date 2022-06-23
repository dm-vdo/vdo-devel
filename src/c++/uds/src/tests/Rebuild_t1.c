// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * Test rebuild after saving the index with a partial chapter 0, and then
 * crashing after writing a full chapter 0 to the volume file.
 *
 * This test demonstrates the failure of ALB-2404 that was seen at a
 * customer site.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "uds.h"

#ifdef TEST_INTERNAL
#include "dory.h"
#endif /* TEST_INTERNAL */

static const char *indexName;

enum { NUM_CHUNKS = 1000 };

/**********************************************************************/
static void postChunks(struct uds_index_session *indexSession,
                       int                       base,
                       int                       count)
{
  long index;
  for (index = base; index < base + count; index++) {
    struct uds_chunk_name chunkName = murmurGenerator(&index, sizeof(index));
    oldPostBlockName(indexSession, NULL, (struct uds_chunk_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
}

/**********************************************************************/
static void fullRebuildTest(void)
{
  initializeOldInterfaces(2000);

  // Create a new index.
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  randomizeUdsNonce(&params);

  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  // Write the base set of 1000 chunks to the index.
  postChunks(indexSession, 0, NUM_CHUNKS);
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(NUM_CHUNKS, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(0, indexStats.posts_found);
  CU_ASSERT_EQUAL(NUM_CHUNKS, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  /*
   * At this point we have a saved volume and index page map that are empty,
   * because we have yet to write a full chapter.  The saved volume index
   * contains 1000 chunk names that are in chapter 0.  Chapter 0 was saved as
   * the open chapter.
   */

  // Open the cleanly saved index.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params, indexSession));
  int numChaptersWritten = atomic_read_acquire(&chapters_written);
  // Write one chapter of chunks.
  unsigned int numBlocksPerChapter = getBlocksPerChapter(indexSession);
  CU_ASSERT(NUM_CHUNKS < numBlocksPerChapter);
  postChunks(indexSession, NUM_CHUNKS, numBlocksPerChapter);
  // Wait for the chapter write to complete.
  while (numChaptersWritten == atomic_read_acquire(&chapters_written)) {
    sleep_for(ms_to_ktime(100));
  }
  // Turn off writing, and do a dirty closing of the index.
  set_dory_forgetful(true);
  UDS_ASSERT_ERROR(-EROFS, uds_close_index(indexSession));
  set_dory_forgetful(false);

  /*
   * Now we have written chapter 0 to the volume.  We have written neither the
   * volume index nor the index page map, and we have deleted the open chapter.
   */

  // Make sure the index will not load.
  UDS_ASSERT_ERROR(-EEXIST, uds_open_index(UDS_NO_REBUILD, &params,
                                           indexSession));
  // Rebuild the index.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));
  // Rewrite the base set of 1000 chunks.
  postChunks(indexSession, 0, NUM_CHUNKS);
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(NUM_CHUNKS, indexStats.posts_found);
  CU_ASSERT_EQUAL(0, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithIndexName(const char *in)
{
  indexName = in;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Full Rebuild", fullRebuildTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Rebuild_t1",
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
