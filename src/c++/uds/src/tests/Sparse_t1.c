// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "index.h"
#include "index-layout.h"
#include "logger.h"
#include "memory-alloc.h"
#include "random.h"
#include "request-queue.h"
#include "testPrototypes.h"

static unsigned int CHAPTERS_PER_VOLUME;
static unsigned int SPARSE_CHAPTERS_PER_VOLUME;
static unsigned int IDEAL_NUM_HASHES_IN_CHAPTER;
static unsigned int NUM_HASHES_IN_CHAPTER;
static unsigned int NUM_HASHES;

// for readability
static const bool DO_UPDATE   = true;
static const bool DONT_UPDATE = false;

static struct uds_chunk_name *hashes;
static struct uds_chunk_data *metas;
static struct configuration  *config;
static struct uds_index      *theIndex;

static struct cond_var       callbackCond;
static struct mutex          callbackMutex;
static unsigned int          callbackCount = 0;
static enum uds_index_region lastLocation;

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

/**
 * Create an index.
 *
 * @param loadHow  the load type
 */
static void createIndex(enum uds_open_index_type openType)
{
  UDS_ASSERT_SUCCESS(make_index(config, openType, NULL, &testCallback,
                                &theIndex));
}

/**********************************************************************/
static void cleanupIndex(void)
{
  free_index(theIndex);
  theIndex = NULL;
}

/**
 * Check whether the most recently generated chunk name might be a chapter
 * index collision with all the previously generated chunk names.
 *
 * @param lastHash  The index of the most recently generated chunk name
 *
 * @return <code>true</code> if the most recent name may be a collision
 **/
static bool searchForChapterIndexCollision(unsigned int lastHash)
{
  struct geometry *geometry = theIndex->volume->geometry;
  unsigned int i;
  for (i = 0; i < lastHash; i++) {
    if (hash_to_chapter_delta_address(&hashes[lastHash], geometry)
        == hash_to_chapter_delta_address(&hashes[i], geometry)) {
      return true;
    }
  }
  return false;
}

/**
 * The suite initialization function.
 **/
static void sparseInitSuite(const char *name)
{
  UDS_ASSERT_SUCCESS(uds_init_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_init_mutex(&callbackMutex));

  struct uds_parameters params = {
    .memory_size = 1,
    .name = name,
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));

  unsigned int zoneCount        = config->zone_count;
  unsigned int RECORDS_PER_PAGE = 128;
  CHAPTERS_PER_VOLUME         = 10;
  SPARSE_CHAPTERS_PER_VOLUME  = 5;
  IDEAL_NUM_HASHES_IN_CHAPTER = 128;
  NUM_HASHES_IN_CHAPTER       = (IDEAL_NUM_HASHES_IN_CHAPTER
                                 - IDEAL_NUM_HASHES_IN_CHAPTER % zoneCount
                                 - zoneCount + 1);
  NUM_HASHES                  = NUM_HASHES_IN_CHAPTER * CHAPTERS_PER_VOLUME;
  resizeSparseConfiguration(config, RECORDS_PER_PAGE * BYTES_PER_RECORD,
                            IDEAL_NUM_HASHES_IN_CHAPTER / RECORDS_PER_PAGE,
                            CHAPTERS_PER_VOLUME, SPARSE_CHAPTERS_PER_VOLUME,
                            2);

  createIndex(UDS_CREATE);

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_HASHES, struct uds_chunk_name, "hashes",
                                  &hashes));

  unsigned int i, j;
  for (i = 0; i < NUM_HASHES; i++) {
    /*
     * Keep picking random chunk names until we find one that isn't a chapter
     * index collision. This prevents us from hitting the very rare case of
     * one non-hook colliding with another in the chapter index, which leads
     * to one of them not being found in cacheHitTest() since UDS doesn't
     * retry the sparse search after a false chapter index hit.
     */
    do {
      createRandomBlockNameInZone(theIndex, i % theIndex->zone_count,
                                  &hashes[i]);
      set_sampling_bytes(&hashes[i], i % 2);
    } while (searchForChapterIndexCollision(i));
  }

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_HASHES, struct uds_chunk_data, "metas",
                                  &metas));
  for (i = 0; i < NUM_HASHES; i++) {
    for (j = 0; j < UDS_METADATA_SIZE; j++) {
      metas[i].data[j] = i;
    }
  }
}

/**
 * The suite cleanup function.
 **/
static void sparseCleanSuite(void)
{
  UDS_FREE(metas);
  UDS_FREE(hashes);
  cleanupIndex();
  free_configuration(config);
  UDS_ASSERT_SUCCESS(uds_destroy_cond(&callbackCond));
  UDS_ASSERT_SUCCESS(uds_destroy_mutex(&callbackMutex));
}

/**********************************************************************/
static void dispatchRequest(struct uds_request          *request,
                            enum uds_index_region        expectedLocation,
                            const struct uds_chunk_data *expectedMetaData)
{
  request->index = theIndex;
  incrementCallbackCount();
  request->unbatched = true;
  enqueue_request(request, STAGE_TRIAGE);
  waitForCallbacks();
  assertLastLocation(expectedLocation);
  if (request->found && (expectedMetaData != NULL)) {
    UDS_ASSERT_BLOCKDATA_EQUAL(expectedMetaData, &request->old_metadata);
  }
}

/**********************************************************************/
static noinline void indexAddAndCheck(unsigned int          hashIndex,
                                      unsigned int          metaInIndex,
                                      enum uds_index_region expectedLocation,
                                      unsigned int          expectedMetaIndex)

{
  // This routine is forced to NOT be inlined, because otherwise the
  // request variable makes the caller's stack frame too large.
  struct uds_request request = {
    .chunk_name   = hashes[hashIndex],
    .new_metadata = metas[metaInIndex],
    .type         = UDS_POST,
  };
  dispatchRequest(&request, expectedLocation, &metas[expectedMetaIndex]);
}

/**********************************************************************/
static void indexAdd(unsigned int index)
{
  indexAddAndCheck(index, index, UDS_LOCATION_UNAVAILABLE, 0);
}

/**********************************************************************/
static noinline void assertLookup(unsigned int          index,
                                  enum uds_index_region expectedLocation,
                                  bool                  update)
{
  // This routine is forced to NOT be inlined, because otherwise the
  // request variable makes the caller's stack frame too large.
  struct uds_request request = {
    .chunk_name = hashes[index],
    .type       = (update == DO_UPDATE) ? UDS_QUERY : UDS_QUERY_NO_UPDATE,
  };
  dispatchRequest(&request, expectedLocation, &metas[index]);
}

/**********************************************************************/
static void fillOpenChapter(uint64_t chapterNumber, unsigned int numAdded)
{
  if (theIndex->zone_count == 1) {
    CU_ASSERT_EQUAL(numAdded, theIndex->zones[0]->open_chapter->size);
  }

  static unsigned int zone = 0;
  for (; numAdded < NUM_HASHES_IN_CHAPTER; ++numAdded) {
    struct uds_request request = { .type = UDS_POST };
    createRandomBlockNameInZone(theIndex, zone, &request.chunk_name);
    createRandomMetadata(&request.new_metadata);
    dispatchRequest(&request, UDS_LOCATION_UNAVAILABLE, NULL);
    zone = (zone + 1) % theIndex->zone_count;
  }

  wait_for_idle_index(theIndex);
  CU_ASSERT_EQUAL(chapterNumber + 1, theIndex->newest_virtual_chapter);
}

/**********************************************************************/
static struct volume_index_record
getTheVolumeIndexRecord(unsigned int hashIndex)
{
  struct volume_index_record record;
  int result = get_volume_index_record(theIndex->volume_index,
                                       &hashes[hashIndex],
                                       &record);
  UDS_ASSERT_SUCCESS(result);
  return record;
}

/**********************************************************************/
static void assertFoundInMI(unsigned int hashIndex)
{
  CU_ASSERT_TRUE(getTheVolumeIndexRecord(hashIndex).is_found);
}

/**********************************************************************/
static void assertNotFoundInMI(unsigned int hashIndex)
{
  CU_ASSERT_FALSE(getTheVolumeIndexRecord(hashIndex).is_found);
}

/**********************************************************************/
static void assertIsHook(unsigned int hashIndex)
{
  assertFoundInMI(hashIndex);
  CU_ASSERT_TRUE((extract_sampling_bytes(&hashes[hashIndex])
                  % config->sparse_sample_rate) == 0);
}

/**********************************************************************/
static bool isHook(unsigned int hashIndex)
{
  return is_volume_index_sample(theIndex->volume_index, &hashes[hashIndex]);
}

/**********************************************************************/
static void assertLocation(unsigned int          hashIndex,
                           enum uds_index_region location,
                           unsigned int          chapterHits,
                           unsigned int          chapterMisses,
                           unsigned int          searchHits)
{
  struct cache_counters before
    = get_sparse_cache_counters(theIndex->volume->sparse_cache);
  assertLookup(hashIndex, location, DONT_UPDATE);
  struct cache_counters after
    = get_sparse_cache_counters(theIndex->volume->sparse_cache);

  if (theIndex->zone_count > 1) {
    // TODO: Understand why the following assertions fail when there is
    //       more than 1 zone.
    return;
  }

  CU_ASSERT_EQUAL(chapterHits,
                  after.sparse_chapters.hits - before.sparse_chapters.hits);
  CU_ASSERT_EQUAL(chapterMisses,
                  after.sparse_chapters.misses - before.sparse_chapters.misses);
  CU_ASSERT_EQUAL(searchHits,
                  after.sparse_searches.hits - before.sparse_searches.hits);
}

/** Tests **/

/**********************************************************************/
static void sparseIndexTest(void)
{
  CU_ASSERT_EQUAL(0, theIndex->newest_virtual_chapter);
  indexAdd(1);
  assertLocation(1, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  indexAdd(2);
  indexAdd(3);
  indexAdd(4);
  assertLookup(1, UDS_LOCATION_IN_OPEN_CHAPTER, DO_UPDATE);
  assertLookup(4, UDS_LOCATION_IN_OPEN_CHAPTER, DO_UPDATE);
  assertLocation(1, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  assertLocation(2, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  assertLocation(3, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  assertLocation(4, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);

  fillOpenChapter(0, 4);
  assertLocation(1, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertLocation(2, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertLookup(1, UDS_LOCATION_IN_DENSE, DO_UPDATE);
  assertLookup(2, UDS_LOCATION_IN_DENSE, DO_UPDATE);
  assertLocation(1, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  assertLocation(2, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);
  assertLocation(3, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertLocation(4, UDS_LOCATION_IN_DENSE, 0, 0, 0);

  fillOpenChapter(1, 2);
  assertLocation(1, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertLocation(2, UDS_LOCATION_IN_DENSE, 0, 0, 0);

  // Should sparsify first chapter (0) and make 3 disappear.
  unsigned int i;
  for (i = 2; i < 5; i++) {
    fillOpenChapter(i, 0);
  }
  assertFoundInMI(1);
  assertFoundInMI(2);
  assertNotFoundInMI(3);
  assertIsHook(4);

  fillOpenChapter(5, 0);
  assertNotFoundInMI(1);
  assertIsHook(2);
  // barrier miss, cache update, hook hit (+1/+1)
  assertLocation(2, UDS_LOCATION_IN_SPARSE, 1, 1, 1);
  assertNotFoundInMI(3);
  // not in sparse cache yet
  assertLookup(3, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertIsHook(4);
  // barrier miss, cache update, hook hit (+1/+1)
  assertLocation(4, UDS_LOCATION_IN_SPARSE, 1, 1, 1);
  // search hit (+0/+1)
  assertLocation(3, UDS_LOCATION_IN_SPARSE, 0, 0, 1);

  assertLookup(3, UDS_LOCATION_IN_SPARSE, DO_UPDATE);
  assertLocation(3, UDS_LOCATION_IN_OPEN_CHAPTER, 0, 0, 0);

  fillOpenChapter(6, 1);
  assertNotFoundInMI(1);
  assertIsHook(2);
  // barrier hit (+1/0), hook hit (+1/+1)
  assertLocation(2, UDS_LOCATION_IN_SPARSE, 2, 0, 1);
  assertFoundInMI(3);
  assertLocation(3, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertIsHook(4);
  // barrier hit (+1/0), hook hit (+1/+1)
  assertLocation(4, UDS_LOCATION_IN_SPARSE, 2, 0, 1);

  // Test wrap-around, sparsifying.
  for (i = 7; i < 9; i++) {
    fillOpenChapter(i, 0);
  }
  assertNotFoundInMI(1);
  assertIsHook(2);
  // barrier hit (+1/0), hook hit (+1/+1)
  assertLocation(2, UDS_LOCATION_IN_SPARSE, 2, 0, 1);
  assertFoundInMI(3);
  assertLocation(3, UDS_LOCATION_IN_DENSE, 0, 0, 0);
  assertIsHook(4);
  // barrier hit (+1/0), hook hit (+1/+1)
  assertLocation(4, UDS_LOCATION_IN_SPARSE, 2, 0, 1);

  fillOpenChapter(9, 0);
  assertNotFoundInMI(1);
  assertIsHook(2);
  assertFoundInMI(3);
  assertNotFoundInMI(4);

  fillOpenChapter(10, 0);
  assertNotFoundInMI(1);
  assertNotFoundInMI(2);
  assertNotFoundInMI(3);
  assertNotFoundInMI(4);
}

/**********************************************************************/
static void cacheHitTest(void)
{
  unsigned int i;
  for (i = 0; i < NUM_HASHES - 1; i++) {
    struct volume_index_record record = getTheVolumeIndexRecord(i);
    if (record.is_found) {
      /*
       * We're about to create a volume index collision, which may break the
       * logic in the rest of this test since it can cause the sparse cache to
       * be filled prematurely. This is a rare occurence (an collision in
       * 60-odd names), so just bail on this test case this time.
       */
      uds_log_info("cacheHitTest bypassed because of volume index collision");
      return;
    }

    indexAdd(i);
  }

  // Cache is empty. Will not find any non hook entries in sparse chapters.
  for (i = 0; i < NUM_HASHES / 2; ++i) {
    if (!isHook(i)) {
      assertLookup(i, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
    }
  }

  // cache will be filled here by finding hook entries in sparse chapters.
  unsigned int chapter;
  for (chapter = 0; chapter < SPARSE_CHAPTERS_PER_VOLUME; ++chapter) {
    for (i = chapter * NUM_HASHES_IN_CHAPTER;
         i < (chapter + 1) * NUM_HASHES_IN_CHAPTER; ++i) {
      if (isHook(i)) {
        assertLookup(i, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
      }
    }
  }

  // Cache will be hit here, so we should find all entries in sparse chapters
  for (chapter = 0; chapter < SPARSE_CHAPTERS_PER_VOLUME; ++chapter) {
    for (i = chapter * NUM_HASHES_IN_CHAPTER;
         i < (chapter + 1) * NUM_HASHES_IN_CHAPTER; ++i) {
      if (!isHook(i)) {
        assertLookup(i, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
      }
    }
  }
}

/**********************************************************************/
static void saveLoadTest(void)
{
  uint64_t newestVirtualChapter = theIndex->newest_virtual_chapter;
  uint64_t oldestVirtualChapter = theIndex->oldest_virtual_chapter;

  // Have to add so few entries that they fit in a single chapter,
  // to test saving and loading of the open chapter.
  unsigned int hashesToAdd = NUM_HASHES_IN_CHAPTER / 4 * 3;
  unsigned int i;
  for (i = 0; i < hashesToAdd; i++) {
    indexAdd(i);
  }
  UDS_ASSERT_SUCCESS(save_index(theIndex));

  cleanupIndex();
  createIndex(UDS_NO_REBUILD);

  // Change the metadata of the hashes in the open chapter
  // and verify we get the right old metadata anyhow.
  for (i = 0; i < hashesToAdd; i++) {
    indexAddAndCheck(i, 0, UDS_LOCATION_IN_OPEN_CHAPTER, i);
  }
  CU_ASSERT_EQUAL(newestVirtualChapter, theIndex->newest_virtual_chapter);
  CU_ASSERT_EQUAL(oldestVirtualChapter, theIndex->oldest_virtual_chapter);
  CU_ASSERT_EQUAL(SPARSE_CHAPTERS_PER_VOLUME,
                  theIndex->volume->geometry->sparse_chapters_per_volume);

  cleanupIndex();
  createIndex(UDS_CREATE);

  // Verify that the old hashes got blown away
  for (i = 0; i < hashesToAdd; i++) {
    indexAdd(i);
  }
}

/**********************************************************************/
static void sparseRebuildTest(void)
{
  unsigned int chapter, i;
  for (chapter = 0;
       chapter < theIndex->volume->geometry->chapters_per_volume - 1;
       chapter++) {
    for (i = 0; i < NUM_HASHES_IN_CHAPTER; i++) {
      indexAdd((chapter * NUM_HASHES_IN_CHAPTER) + i);
    }
  }

  UDS_ASSERT_SUCCESS(save_index(theIndex));
  cleanupIndex();
  createIndex(UDS_NO_REBUILD);

  CU_ASSERT_EQUAL(theIndex->volume->geometry->chapters_per_volume - 1,
                  theIndex->newest_virtual_chapter);
  CU_ASSERT_EQUAL(0, theIndex->oldest_virtual_chapter);
  CU_ASSERT_EQUAL(SPARSE_CHAPTERS_PER_VOLUME,
                  theIndex->volume->geometry->sparse_chapters_per_volume);

  UDS_ASSERT_SUCCESS(discard_index_state_data(theIndex->layout));
  cleanupIndex();
  createIndex(UDS_LOAD);

  // Verify all the dense data is still there
  enum uds_index_region loc = UDS_LOCATION_IN_DENSE;
  for (chapter = SPARSE_CHAPTERS_PER_VOLUME;
       chapter < theIndex->volume->geometry->chapters_per_volume - 1;
       chapter++) {

    for (i = 0; i < NUM_HASHES_IN_CHAPTER; i++) {
      indexAddAndCheck((chapter * NUM_HASHES_IN_CHAPTER) + i,
                       (chapter * NUM_HASHES_IN_CHAPTER) + i, loc,
                       (chapter * NUM_HASHES_IN_CHAPTER) + i);
    }
  }
}

/**********************************************************************/
static const CU_TestInfo sparseTests[] = {
  { "Sparse Index",   sparseIndexTest   },
  { "Cache Hit",      cacheHitTest      },
  { "Save Load",      saveLoadTest      },
  { "Sparse Rebuild", sparseRebuildTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Sparse_t1",
  .initializerWithIndexName = sparseInitSuite,
  .cleaner                  = sparseCleanSuite,
  .tests                    = sparseTests,
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
