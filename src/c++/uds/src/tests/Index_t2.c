// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 *
 * Tests that exercise index rebuilding.
 */

#include "albtest.h"
#include "assertions.h"
#include "index.h"
#include "memory-alloc.h"
#include "open-chapter.h"
#include "testPrototypes.h"
#include "testRequests.h"

typedef struct indexTestData {
  struct uds_index       *index;
  struct uds_record_name *hashes;
  struct uds_record_data *metas;
  unsigned int            totalRecords;
  unsigned int            recordsPerChapter;
} IndexTestData;

static struct block_device *testDevice;
static IndexTestData testData;
static unsigned int NUM_CHAPTERS;

struct configuration *testConfig;
struct configuration *denseConfig;
struct configuration *sparseConfig;

/**
 * The suite initialization function.
 **/
static void indexInitSuite(struct block_device *bdev)
{
  testDevice = bdev;

  NUM_CHAPTERS = 8;

  // Set up the geometry and config for dense index testing
  struct uds_parameters params = {
    .memory_size = 1,
    .bdev = testDevice,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &denseConfig));
  unsigned int zoneCount = denseConfig->zone_count;
  if (zoneCount >= 7) {
    // Need more delta-lists when we have many zones, so up the record count.
    NUM_CHAPTERS *= zoneCount;
  }
  resizeDenseConfiguration(denseConfig, 4096, 32, NUM_CHAPTERS);

  // Set up the geometry and config for sparse index testing
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &sparseConfig));
  resizeSparseConfiguration(sparseConfig,
                            sparseConfig->geometry->bytes_per_page / 8,
                            64, NUM_CHAPTERS, NUM_CHAPTERS / 2, 2);

  testConfig = denseConfig;
  initialize_test_requests();
}

/**
 * The suite cleanup function.
 **/
static void indexCleanSuite(void)
{
  uninitialize_test_requests();
  uds_free_index(testData.index);
  uds_free(testData.metas);
  uds_free(testData.hashes);
  uds_free_configuration(denseConfig);
  uds_free_configuration(sparseConfig);
}

/**
 * Create of the index and test data
 **/
static void initTestData(unsigned int numChapters, unsigned int collisionFreq)
{
  UDS_ASSERT_SUCCESS(uds_make_index(testConfig, UDS_CREATE, NULL, NULL, &testData.index));

  // Create a lot of records. Will use the metadata to store chapter number.
  testData.recordsPerChapter
    = testData.index->volume->geometry->records_per_chapter;
  testData.totalRecords = testData.recordsPerChapter * numChapters;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(testData.totalRecords,
                                  struct uds_record_name,
                                  __func__, &testData.hashes));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(testData.totalRecords,
                                  struct uds_record_data,
                                  __func__, &testData.metas));
  uint64_t i;
  for (i = 0; i < testData.totalRecords; i++) {
    if ((i != 0) && (collisionFreq != 0) && ((i % collisionFreq) == 0)) {
      createCollidingBlock(&testData.hashes[i-1], &testData.hashes[i]);
    } else {
      createRandomBlockName(&testData.hashes[i]);
    }
  }
}

/**
 * Preload some data into the index
 **/
static void preloadData(uint64_t numChapters)
{
  uint64_t chapter;
  for (chapter = 0; chapter < numChapters; chapter++) {
    fillChapterRandomly(testData.index);
  }
}

/**
 * Add the data in testData to the index.
 */
static void addData(bool shouldExist)
{
  struct uds_index *index = testData.index;
  unsigned int i;
  for (i = 0; i < testData.totalRecords; i++) {
    unsigned int zoneNumber = uds_get_volume_index_zone(index->volume_index, &testData.hashes[i]);
    struct index_zone *zone = index->zones[zoneNumber];
    uint64_t chapter = zone->newest_virtual_chapter;

    memcpy(&testData.metas[i].data, &chapter, sizeof(chapter));
    struct uds_request request = {
      .record_name  = testData.hashes[i],
      .new_metadata = testData.metas[i],
      .zone_number  = zoneNumber,
      .type         = UDS_UPDATE,
    };
    verify_test_request(index, &request, shouldExist, NULL);

    // If this request closed the chapter, wait for all zones to update.
    if (zone->newest_virtual_chapter > chapter) {
      uds_wait_for_idle_index(index);
    }
  }

  /*
   * Anything in the open chapter will be discarded and then not found after
   * a rebuild. If we only have one zone then we can assure that we don't
   * add open chapter records by counting carefully. For more than one zone
   * we need to force a chapter close after adding the records we care about.
   */
  if (index->zone_count == 1) {
    CU_ASSERT_EQUAL(0, index->zones[0]->open_chapter->size);
  } else {
    fillChapterRandomly(index);
  }
}

/**********************************************************************/
static void queryDataAndCheck(const struct uds_record_name *hashData,
                              const struct uds_record_data *expectedMetaData)
{
  struct uds_request request = {
    .record_name = *hashData,
    .type        = UDS_QUERY_NO_UPDATE,
  };
  verify_test_request(testData.index, &request, true, expectedMetaData);
}

/**
 * Verify the data in testData is in the index
 */
static void verifyData(unsigned int expectedLost)
{
  struct uds_index *index = testData.index;

  unsigned int recordsLost = 0;
  unsigned int i;
  for (i = 0; i < testData.totalRecords; i++) {
    uint64_t metaChapter;
    memcpy(&metaChapter, &testData.metas[i].data, sizeof(metaChapter));

    // We won't find any records in chapters that have been forgotten
    // or records that were in the open chapter before a rebuild.
    unsigned int zoneNum = uds_get_volume_index_zone(index->volume_index, &testData.hashes[i]);
    if ((metaChapter < index->zones[zoneNum]->oldest_virtual_chapter)
        || (metaChapter == index->zones[zoneNum]->newest_virtual_chapter)) {
      recordsLost++;
      continue;
    }

    // We cannot expect to find entries in chapters that are sparse.
    // TODO: we can if they are hooks.
    if (uds_is_chapter_sparse(index->volume->geometry,
                              index->oldest_virtual_chapter,
                              index->newest_virtual_chapter,
                              metaChapter)) {
      continue;
    }

    // First make sure it is found in the chapter we expect.
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(uds_get_volume_index_record(index->volume_index,
                                                   &testData.hashes[i],
                                                   &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(metaChapter, record.virtual_chapter);

    // Now get the record and check it.
    queryDataAndCheck(&testData.hashes[i], &testData.metas[i]);
  }

  if (index->zone_count == 1) {
    CU_ASSERT_EQUAL(expectedLost, recordsLost);
  }
}

/**
 * Rebuild the volume index and verify its state
 */
static void rebuildIndex(void)
{
  struct uds_index *index = testData.index;

  // Wait for the chapter writer to finish
  uds_wait_for_idle_index(index);

  uint64_t oldOldestVirtualChapter = index->oldest_virtual_chapter;
  uint64_t oldNewestVirtualChapter = index->newest_virtual_chapter;

  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  uds_free_index(index);
  testData.index = NULL;

  // Rebuild the volume index.
  UDS_ASSERT_SUCCESS(uds_make_index(testConfig, UDS_LOAD, NULL, NULL, &index));
  testData.index = index;

  CU_ASSERT_EQUAL(oldOldestVirtualChapter, index->oldest_virtual_chapter);
  unsigned int i;
  for (i = 0; i < index->zone_count; ++i) {
    CU_ASSERT_EQUAL(index->oldest_virtual_chapter,
                    index->zones[i]->oldest_virtual_chapter);
    CU_ASSERT_EQUAL(index->newest_virtual_chapter,
                    index->zones[i]->newest_virtual_chapter);
  }
  CU_ASSERT_EQUAL(oldNewestVirtualChapter, index->newest_virtual_chapter);
}

/** Tests */

/**********************************************************************/
static void fullVolumeZeroStartTest(void)
{
  initTestData(NUM_CHAPTERS, 0);
  CU_ASSERT_TRUE(testData.index->volume->geometry->index_pages_per_chapter
                   > 1);
  addData(false);
  rebuildIndex();
  verifyData(testData.recordsPerChapter);
}

/**********************************************************************/
static void fullVolumeOneStartTest(void)
{
  initTestData(NUM_CHAPTERS, 0);
  preloadData(NUM_CHAPTERS - 2);
  addData(false);
  rebuildIndex();
  verifyData(testData.recordsPerChapter);
}

/**********************************************************************/
static void partialVolumeZeroStartTest(void)
{
  initTestData(NUM_CHAPTERS - 1, 0);
  addData(false);
  rebuildIndex();
  verifyData(0);
}

/**********************************************************************/
static void partialVolumeOneStartTest(void)
{
  initTestData(NUM_CHAPTERS - 1, 0);
  preloadData(NUM_CHAPTERS - 2);
  addData(false);
  rebuildIndex();
  verifyData(0);
}

/**********************************************************************/
static void reinsertTest(void)
{
  initTestData(1, 0);

  uint64_t newestVirtualChapter = testData.index->newest_virtual_chapter;
  addData(false);

  // Add same entries but to the next chapter, note this updates the
  // test data metadatas to the new chapter as well.
  addData(true);

  // Make sure we're at the next chapter.
  CU_ASSERT_NOT_EQUAL(newestVirtualChapter,
                      testData.index->newest_virtual_chapter);

  rebuildIndex();
  verifyData(0);
}

/**********************************************************************/
static void badLoadTest(void)
{
  initTestData(NUM_CHAPTERS - 1, 0);

  // Add data and save it.
  addData(false);
  UDS_ASSERT_SUCCESS(uds_save_index(testData.index));

  UDS_ASSERT_SUCCESS(discard_index_state_data(testData.index->layout));

  uds_free_index(testData.index);
  testData.index = NULL;

  // Try to load the index for real, this should fail since the load files
  // are missing and we are not permitting rebuild.
  UDS_ASSERT_ERROR2(ENOENT, UDS_INDEX_NOT_SAVED_CLEANLY,
                    uds_make_index(testConfig, UDS_NO_REBUILD, NULL, NULL, &testData.index));

  // Try to load the index for real, this time allow rebuild
  UDS_ASSERT_SUCCESS(uds_make_index(testConfig, UDS_LOAD, NULL, NULL, &testData.index));

  verifyData(0);
}

/**********************************************************************/
static void testMissingOpenChapter(bool shouldAddData)
{
  initTestData(NUM_CHAPTERS - 1, 0);

  // Add data and save it.
  if (shouldAddData) {
    addData(false);
  }
  UDS_ASSERT_SUCCESS(uds_save_index(testData.index));

  UDS_ASSERT_SUCCESS(uds_discard_open_chapter(testData.index->layout));
  uds_free_index(testData.index);
  testData.index = NULL;

  // Try to load the index for real, this should fail since one of the
  // components is missing.
  UDS_ASSERT_ERROR3(ENOENT, UDS_INDEX_NOT_SAVED_CLEANLY, UDS_CORRUPT_DATA,
                    uds_make_index(testConfig, UDS_NO_REBUILD, NULL, NULL, &testData.index));

  // Try to load the index for real, this time allow rebuild
  UDS_ASSERT_SUCCESS(uds_make_index(testConfig, UDS_LOAD, NULL, NULL, &testData.index));

  if (shouldAddData) {
    verifyData(0);
  }
}

/**********************************************************************/
static void missingOpenChapterTestEmpty(void)
{
  testMissingOpenChapter(false);
}

/**********************************************************************/
static void missingOpenChapterTest(void)
{
  testMissingOpenChapter(true);
}

/**********************************************************************/
static void collisionsTest(void)
{
  initTestData(1, 4);

  uint64_t newestVirtualChapter
    = testData.index->newest_virtual_chapter;

  addData(false);
  // Add same entries but to the next chapter, note this updates the
  // test data metadatas to the new chapter as well.
  addData(true);

  // Make sure we're at the next chapter.
  CU_ASSERT_NOT_EQUAL(newestVirtualChapter,
                      testData.index->newest_virtual_chapter);

  rebuildIndex();
  verifyData(0);

}

/**********************************************************************/
static void sparseFullVolumeZeroStartTest(void)
{
  testConfig = sparseConfig;
  fullVolumeZeroStartTest();
}

/**********************************************************************/
static void sparseFullVolumeOneStartTest(void)
{
  testConfig = sparseConfig;
  fullVolumeOneStartTest();
}

/**********************************************************************/
static void sparsePartialVolumeZeroStartTest(void)
{
  testConfig = sparseConfig;
  partialVolumeZeroStartTest();
}

/**********************************************************************/
static void sparsePartialVolumeOneStartTest(void)
{
  testConfig = sparseConfig;
  partialVolumeOneStartTest();
}

/**********************************************************************/
static const CU_TestInfo indexTests[] = {
  { "Dense Full Volume, Starting 0",        fullVolumeZeroStartTest          },
  { "Dense Full Volume, Starting Last",     fullVolumeOneStartTest           },
  { "Dense Partial Volume, Starting 0",     partialVolumeZeroStartTest       },
  { "Dense Partial Volume, Starting Last",  partialVolumeOneStartTest        },
  { "Reinsert",                             reinsertTest                     },
  { "Bad Load Test",                        badLoadTest                      },
  { "Missing Open Chapter Test",            missingOpenChapterTest           },
  { "Missing Empty Open Chapter",           missingOpenChapterTestEmpty      },
  { "Collisions Test",                      collisionsTest                   },
  { "Sparse Full Volume, Starting 0",       sparseFullVolumeZeroStartTest    },
  { "Sparse Full Volume, Starting Last",    sparseFullVolumeOneStartTest     },
  { "Sparse Partial Volume, Starting 0",    sparsePartialVolumeZeroStartTest },
  { "Sparse Partial Volume, Starting Last", sparsePartialVolumeOneStartTest  },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "Index_t2",
  .initializerWithBlockDevice = indexInitSuite,
  .cleaner                    = indexCleanSuite,
  .tests                      = indexTests, // List of suite tests
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
