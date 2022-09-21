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
#include "request-queue.h"
#include "testPrototypes.h"

static unsigned int CHAPTERS_PER_VOLUME        = 10;
static unsigned int SPARSE_CHAPTERS_PER_VOLUME = 5;
static unsigned int MAX_RECORDS_PER_CHAPTER    = 128;
static unsigned int RECORDS_PER_PAGE           = 128;
static unsigned int SPARSE_SAMPLE_RATE         = 2;

// for readability
static const bool DO_UPDATE   = true;
static const bool DONT_UPDATE = false;

static unsigned int recordsPerChapter;
static unsigned int totalRecords;

static struct uds_record_name *hashes;
static struct uds_chunk_data  *metas;
static struct configuration   *config;
static struct uds_index       *theIndex;

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

/**********************************************************************/
static struct volume_index_record
getTheVolumeIndexRecord(unsigned int hashIndex)
{
  struct volume_index_record record;
  UDS_ASSERT_SUCCESS(get_volume_index_record(theIndex->volume_index,
                                             &hashes[hashIndex],
                                             &record));
  return record;
}

/**********************************************************************/
static void assertFoundInVolumeIndex(unsigned int hashIndex)
{
  CU_ASSERT_TRUE(getTheVolumeIndexRecord(hashIndex).is_found);
}

/**********************************************************************/
static void assertNotFoundInVolumeIndex(unsigned int hashIndex)
{
  CU_ASSERT_FALSE(getTheVolumeIndexRecord(hashIndex).is_found);
}

/**********************************************************************/
static bool isHook(unsigned int hashIndex)
{
  return is_volume_index_sample(theIndex->volume_index, &hashes[hashIndex]);
}

/**********************************************************************/
static void assertIsHook(unsigned int hashIndex)
{
  CU_ASSERT_TRUE(isHook(hashIndex));
}

/**
 * Check whether the most recently generated record name might be a
 * volume index collision or a chapter index collision with all the
 * previously generated record names.
 *
 * @param lastHash  The index of the most recently generated record name
 *
 * @return <code>true</code> if the most recent name may be a collision
 **/
static bool searchForCollisions(unsigned int lastHash)
{
  struct geometry *geometry = theIndex->volume->geometry;
  unsigned int i;
  for (i = 0; i < lastHash; i++) {
    if (getTheVolumeIndexRecord(i).is_found) {
      return true;
    }

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

  unsigned int zoneCount = config->zone_count;
  recordsPerChapter = (MAX_RECORDS_PER_CHAPTER
                       - (MAX_RECORDS_PER_CHAPTER % zoneCount)
                       - zoneCount + 1);
  totalRecords = recordsPerChapter * CHAPTERS_PER_VOLUME;

  resizeSparseConfiguration(config, RECORDS_PER_PAGE * BYTES_PER_RECORD,
                            MAX_RECORDS_PER_CHAPTER / RECORDS_PER_PAGE,
                            CHAPTERS_PER_VOLUME, SPARSE_CHAPTERS_PER_VOLUME,
                            SPARSE_SAMPLE_RATE);
  createIndex(UDS_CREATE);

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(totalRecords,
                                  struct uds_record_name,
                                  "hashes",
                                  &hashes));

  unsigned int i, j;
  for (i = 0; i < totalRecords; i++) {
    /*
     * Keep picking random record names until we find one that isn't a chapter
     * index collision. This prevents us from hitting the very rare case of
     * one non-hook colliding with another in the chapter index, which leads
     * to one of them not being found in cacheHitTest() since UDS doesn't
     * retry the sparse search after a false chapter index hit.
     *
     * Also ensure that the new name is not a volume index collision since that
     * can cause the sparse cache to fill up sooner than we expect.
     *
     * Finally, tweak the hashes so that even-numbered indexes are hooks, and
     * odd ones are not hooks.
     */
    do {
      createRandomBlockNameInZone(theIndex, i % theIndex->zone_count,
                                  &hashes[i]);
      set_sampling_bytes(&hashes[i], i % SPARSE_SAMPLE_RATE);
    } while (searchForCollisions(i));
  }

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(totalRecords, struct uds_chunk_data, "metas",
                                  &metas));
  for (i = 0; i < totalRecords; i++) {
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
    .record_name  = hashes[hashIndex],
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
    .record_name = hashes[index],
    .type        = (update == DO_UPDATE) ? UDS_QUERY : UDS_QUERY_NO_UPDATE,
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
  for (; numAdded < recordsPerChapter; ++numAdded) {
    struct uds_request request = { .type = UDS_POST };
    createRandomBlockNameInZone(theIndex, zone, &request.record_name);
    createRandomMetadata(&request.new_metadata);
    dispatchRequest(&request, UDS_LOCATION_UNAVAILABLE, NULL);
    zone = (zone + 1) % theIndex->zone_count;
  }

  wait_for_idle_index(theIndex);
  CU_ASSERT_EQUAL(chapterNumber + 1, theIndex->newest_virtual_chapter);
}

/**********************************************************************/
static void sparseIndexTest(void)
{
  // Records 2 and 4 are hooks; records 1 and 3 are not hooks.
  CU_ASSERT_EQUAL(0, theIndex->newest_virtual_chapter);
  assertIsHook(2);
  assertIsHook(4);

  // Add all four records to chapter 0, the open chapter.
  indexAdd(1);
  assertLookup(1, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  indexAdd(2);
  indexAdd(3);
  indexAdd(4);
  assertLookup(1, UDS_LOCATION_IN_OPEN_CHAPTER, DO_UPDATE);
  assertLookup(4, UDS_LOCATION_IN_OPEN_CHAPTER, DO_UPDATE);
  assertLookup(1, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  assertLookup(2, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  assertLookup(3, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  assertLookup(4, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);

  // Advance the open chapter to chapter 1, and put records 1 and 2 in
  // the new open chpater.
  fillOpenChapter(0, 4);
  assertLookup(1, UDS_LOCATION_IN_DENSE, DO_UPDATE);
  assertLookup(2, UDS_LOCATION_IN_DENSE, DO_UPDATE);
  assertLookup(1, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  assertLookup(2, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);
  assertLookup(3, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertLookup(4, UDS_LOCATION_IN_DENSE, DONT_UPDATE);

  // Advance the open chapter to chapter 2.
  fillOpenChapter(1, 2);
  assertLookup(1, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertLookup(2, UDS_LOCATION_IN_DENSE, DONT_UPDATE);

  // Fill enough chapters to sparsify chapter 0 and make record 3
  // disappear.
  unsigned int i;
  for (i = 2; i < SPARSE_CHAPTERS_PER_VOLUME; i++) {
    fillOpenChapter(i, 0);
  }
  assertFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertFoundInVolumeIndex(4);

  // Advance one more chapter to sparsify chapter 1 and make record 1
  // disappear.
  fillOpenChapter(SPARSE_CHAPTERS_PER_VOLUME, 0);
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);

  // Looking up record 2 will pull chapter 1 in to the sparse cache,
  // allowing us to find record 1 again.
  assertFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertFoundInVolumeIndex(4);

  // Looking up record 4 will pull chapter 0 into the sparse cache,
  // allowing us to find record 3 again. Move record 3 into the
  // current open chapter (5) so it's in the dense region.
  assertLookup(4, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_IN_SPARSE, DO_UPDATE);
  assertLookup(3, UDS_LOCATION_IN_OPEN_CHAPTER, DONT_UPDATE);

  // Advance the open chapter again.
  fillOpenChapter(SPARSE_CHAPTERS_PER_VOLUME + 1, 1);
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertFoundInVolumeIndex(4);
  assertLookup(4, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);

  // Advance to the end of the volume.
  for (i = SPARSE_CHAPTERS_PER_VOLUME + 2;
       i < CHAPTERS_PER_VOLUME - 1;
       i++) {
    fillOpenChapter(i, 0);
  }
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertFoundInVolumeIndex(4);
  assertLookup(4, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);

  // Advance again, invalidating chapter 0. This removes it from the
  // volume index and from the sparse cache, and invalidates record 4.
  fillOpenChapter(CHAPTERS_PER_VOLUME - 1, 0);
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
  assertFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_IN_DENSE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(4);
  assertLookup(4, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);

  // Advance again, invalidating chapter 1. This invalidates records 1
  // and 2. Record 3 is still in a valid chapter, but that chapter has
  // just been sparsified so we can't find record 3 any more, either.
  fillOpenChapter(CHAPTERS_PER_VOLUME, 0);
  assertNotFoundInVolumeIndex(1);
  assertLookup(1, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(2);
  assertLookup(2, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(3);
  assertLookup(3, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
  assertNotFoundInVolumeIndex(4);
  assertLookup(4, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
}

/**********************************************************************/
static void cacheHitTest(void)
{
  unsigned int i;
  for (i = 0; i < totalRecords - 1; i++) {
    indexAdd(i);
  }

  // Cache is empty. Will not find any non hook entries in sparse chapters.
  for (i = 0; i < (SPARSE_CHAPTERS_PER_VOLUME * recordsPerChapter); ++i) {
    if (!isHook(i)) {
      assertLookup(i, UDS_LOCATION_UNAVAILABLE, DONT_UPDATE);
    }
  }

  // cache will be filled here by finding hook entries in sparse chapters.
  for (i = 0; i < (SPARSE_CHAPTERS_PER_VOLUME * recordsPerChapter); ++i) {
    if (isHook(i)) {
      assertLookup(i, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
    }
  }

  // Cache will be hit here, so we should find all entries in sparse chapters
  for (i = 0; i < (SPARSE_CHAPTERS_PER_VOLUME * recordsPerChapter); ++i) {
    if (!isHook(i)) {
      assertLookup(i, UDS_LOCATION_IN_SPARSE, DONT_UPDATE);
    }
  }
}

/**********************************************************************/
static void sparseRebuildTest(void)
{
  unsigned int chapter, i;
  for (chapter = 0; chapter < CHAPTERS_PER_VOLUME - 1; chapter++) {
    for (i = 0; i < recordsPerChapter; i++) {
      indexAdd((chapter * recordsPerChapter) + i);
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
  enum uds_index_region location = UDS_LOCATION_IN_DENSE;
  for (chapter = SPARSE_CHAPTERS_PER_VOLUME;
       chapter < CHAPTERS_PER_VOLUME - 1;
       chapter++) {

    for (i = 0; i < recordsPerChapter; i++) {
      indexAddAndCheck((chapter * recordsPerChapter) + i,
                       (chapter * recordsPerChapter) + i,
                       location,
                       (chapter * recordsPerChapter) + i);
    }
  }
}

/**********************************************************************/
static const CU_TestInfo sparseTests[] = {
  { "Sparse Index",   sparseIndexTest   },
  { "Cache Hit",      cacheHitTest      },
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
