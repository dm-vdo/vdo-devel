// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "volume-index-ops.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

enum {
  // Used for an index that wants only a single delta list
  SINGLE_CHAPTERS = 8,

  // Used for an index that wants lots of delta lists
  NUM_CHAPTERS = (1 << 10),
  MAX_CHAPTER = NUM_CHAPTERS - 1,
};

static unsigned int savedMinVolumeIndexDeltaLists;

/**********************************************************************/
static void volumeIndexInit(void)
{
  // This test uses one delta list for simplicity.
  savedMinVolumeIndexDeltaLists = min_volume_index_delta_lists;
  min_volume_index_delta_lists = 1;
}

/**********************************************************************/
static void volumeIndexCleanup(void)
{
  min_volume_index_delta_lists = savedMinVolumeIndexDeltaLists;
}

/**********************************************************************/
static void fillInAddress(struct uds_chunk_name *name, unsigned int addr)
{
  set_volume_index_bytes(name, addr);
}

/**********************************************************************/
static void insertRandomlyNamedBlock(struct volume_index   *volumeIndex,
                                     struct uds_chunk_name *name,
                                     uint64_t               chapter)
{
  createRandomBlockName(name);
  struct volume_index_record record;
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, name, &record));
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, chapter));
}

/**********************************************************************/
static struct configuration *makeTestConfig(int numChapters)
{
  struct configuration *config;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct configuration, __func__, &config));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct geometry, __func__,
                                  &config->geometry));
  config->volume_index_mean_delta = DEFAULT_VOLUME_INDEX_MEAN_DELTA;
  config->zone_count = 1;
  config->geometry->chapters_per_volume = numChapters;
  config->geometry->records_per_chapter = 16;
  return config;
}

/**********************************************************************/
static void
getVolumeIndexStatsDenseOnly(struct volume_index       *volumeIndex,
                             struct volume_index_stats *denseStats)
{
  struct volume_index_stats sparseStats;
  get_volume_index_stats(volumeIndex, denseStats, &sparseStats);
}

/**********************************************************************/
static void initializationTest(void)
{
  struct volume_index *volumeIndex;

  // Expect this to succeed.
  struct configuration *config = makeTestConfig(NUM_CHAPTERS);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**********************************************************************/
static void basicTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_record record;
  struct volume_index_stats *volumeStats;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct volume_index_stats, __func__,
                                  &volumeStats));

  // Make a volume index with only 1 delta list
  struct configuration *config = makeTestConfig(SINGLE_CHAPTERS);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  CU_ASSERT_EQUAL(get_volume_index_memory_used(volumeIndex), 0);
  getVolumeIndexStatsDenseOnly(volumeIndex, volumeStats);
  CU_ASSERT_EQUAL(volumeStats->record_count, 0);
  CU_ASSERT_EQUAL(volumeStats->discard_count, 0);
  CU_ASSERT_EQUAL(volumeStats->num_lists, 1);

  // Make chunk names that use keys 0, 1 and 2
  struct uds_chunk_name name0, name1, name2;
  createRandomBlockName(&name0);
  fillInAddress(&name0, 0);
  createRandomBlockName(&name1);
  fillInAddress(&name1, 1);
  createRandomBlockName(&name2);
  fillInAddress(&name2, 2);

  // Should not find a record with key 0 in an empty index
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name0, &record));
  CU_ASSERT_FALSE(record.is_found);
  CU_ASSERT_EQUAL(get_volume_index_memory_used(volumeIndex), 0);
  getVolumeIndexStatsDenseOnly(volumeIndex, volumeStats);
  CU_ASSERT_EQUAL(volumeStats->record_count, 0);
  CU_ASSERT_EQUAL(volumeStats->discard_count, 0);

  // Insert a record with key 1
  uint64_t chapter1 = 0;
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, chapter1));
  CU_ASSERT_NOT_EQUAL(get_volume_index_memory_used(volumeIndex), 0);
  getVolumeIndexStatsDenseOnly(volumeIndex, volumeStats);
  CU_ASSERT_EQUAL(volumeStats->record_count, 1);
  CU_ASSERT_EQUAL(volumeStats->discard_count, 0);

  // Should not find a record with key 0
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name0, &record));
  CU_ASSERT_FALSE(record.is_found);

  // Should find a record with key 1
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_FALSE(record.is_collision);
  CU_ASSERT_EQUAL(record.virtual_chapter, chapter1);

  // Should not find a record with key 2
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name2, &record));
  CU_ASSERT_FALSE(record.is_found);

  // Remove the record with key 1
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_FALSE(record.is_collision);
  CU_ASSERT_EQUAL(record.virtual_chapter, chapter1);
  UDS_ASSERT_SUCCESS(remove_volume_index_record(&record));
  getVolumeIndexStatsDenseOnly(volumeIndex, volumeStats);
  CU_ASSERT_EQUAL(volumeStats->record_count, 0);
  CU_ASSERT_EQUAL(volumeStats->discard_count, 1);

  // Should not find a record with key 1
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_FALSE(record.is_found);

  CU_ASSERT_EQUAL(get_volume_index_memory_used(volumeIndex), 0);
  getVolumeIndexStatsDenseOnly(volumeIndex, volumeStats);
  CU_ASSERT_EQUAL(volumeStats->record_count, 0);
  CU_ASSERT_EQUAL(volumeStats->discard_count, 1);
  free_volume_index(volumeIndex);
  free_configuration(config);
  UDS_FREE(volumeStats);
}

/**********************************************************************/
static void setChapterTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_record record;

  // Set up a volume index using all chapters from 0 to MAX_CHAPTER
  struct configuration *config = makeTestConfig(NUM_CHAPTERS);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  set_volume_index_open_chapter(volumeIndex, MAX_CHAPTER);

  // Set up to iterate thru chapters in different directions
  uint64_t chapter1 = 0;
  uint64_t chapter2 = MAX_CHAPTER;

  // Insert 2 randomly named blocks
  struct uds_chunk_name name1, name2;
  insertRandomlyNamedBlock(volumeIndex, &name1, chapter1);
  insertRandomlyNamedBlock(volumeIndex, &name2, chapter2);

  // Try out all of the chapter numbers

  for (;;) {
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(record.virtual_chapter, chapter1);

    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name2, &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(record.virtual_chapter, chapter2);

    chapter1++;
    if (chapter2 == 0) {
      break;
    }
    chapter2--;

    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
    CU_ASSERT_TRUE(record.is_found);
    UDS_ASSERT_SUCCESS(set_volume_index_record_chapter(&record, chapter1));

    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name2, &record));
    CU_ASSERT_TRUE(record.is_found);
    UDS_ASSERT_SUCCESS(set_volume_index_record_chapter(&record, chapter2));
  }

  // Try an illegal chapter number.
  CU_ASSERT_EQUAL(set_volume_index_record_chapter(&record, chapter1),
                  UDS_INVALID_ARGUMENT);

  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**
 * Test a trio of entries in the volume index, with chapter invalidation
 *
 * @param addr1  This chunkname is inserted first in chapter 1
 * @param addr2  This chunkname is inserted second in chapter 0.
 *               Then chapter 0 is invalidated.
 * @param addr3  This chunkname is inserted third in chapter 2.
 **/
static void testInvalidateTrio(unsigned int addr1, unsigned int addr2,
                               unsigned int addr3)
{
  struct volume_index *volumeIndex;
  struct volume_index_record record;

  // Set up the volume index to use a single delta list.
  struct configuration *config = makeTestConfig(SINGLE_CHAPTERS);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));

  // Initialize the names
  struct uds_chunk_name name1, name2, name3;
  createRandomBlockName(&name1);
  fillInAddress(&name1, addr1);
  createRandomBlockName(&name2);
  fillInAddress(&name2, addr2);
  createRandomBlockName(&name1);
  fillInAddress(&name3, addr3);

  // These are the chapters we use. Name nameX is inserted into chapter CHx.
  uint64_t CH1 = 1;
  uint64_t CH2 = 0;
  uint64_t CH3 = 2;

  // Advance to CH1 and insert name1
  set_volume_index_open_chapter(volumeIndex, CH1);
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_FALSE(record.is_found);
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, CH1));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_EQUAL(record.virtual_chapter, CH1);

  // Insert name2
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name2, &record));
  CU_ASSERT_FALSE(record.is_found);
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, CH2));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_EQUAL(record.virtual_chapter, CH2);

  // Advance to CH2 + SINGLE_CHAPTERS, invalidating chapter CH2 and expecting
  // that name2 will be removed from index
  set_volume_index_open_chapter(volumeIndex, CH2 + SINGLE_CHAPTERS);

  // Insert name3
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name3, &record));
  CU_ASSERT_FALSE(record.is_found);
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, CH3));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_EQUAL(record.virtual_chapter, CH3);

  // Verify that name1 is present
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_EQUAL(record.virtual_chapter, CH1);

  // Verify that name2 is absent
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name2, &record));
  CU_ASSERT_FALSE(record.is_found);

  // Verify that name3 is present
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name3, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_EQUAL(record.virtual_chapter, CH3);

  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**********************************************************************/
static void invalidate123Test(void)
{
  testInvalidateTrio(1, 2, 3);
}

/**********************************************************************/
static void invalidate132Test(void)
{
  testInvalidateTrio(1, 3, 2);
}

/**********************************************************************/
static void invalidate213Test(void)
{
  testInvalidateTrio(2, 1, 3);
}

/**********************************************************************/
static void invalidate231Test(void)
{
  testInvalidateTrio(2, 3, 1);
}

/**********************************************************************/
static void invalidate312Test(void)
{
  testInvalidateTrio(3, 1, 2);
}

/**********************************************************************/
static void invalidate321Test(void)
{
  testInvalidateTrio(3, 2, 1);
}

/**********************************************************************/
static void advanceForInvalidateChaptersTest(struct volume_index *volumeIndex,
                                             uint64_t *openChapter,
                                             uint64_t chapter)
{
  while (chapter > *openChapter) {
    *openChapter += 1;
    set_volume_index_open_chapter(volumeIndex, *openChapter);
  }
}

/**********************************************************************/
static void insertForInvalidateChaptersTest(struct volume_index *volumeIndex,
                                            unsigned int numChapters,
                                            struct uds_chunk_name *testNames,
                                            uint64_t *testChapters,
                                            uint64_t lowChapter,
                                            uint64_t highChapter,
                                            uint64_t *openChapter)
{
  uint64_t chapter;
  for (chapter = lowChapter; chapter <= highChapter; chapter++) {
    unsigned int index = chapter & (numChapters - 1);
    advanceForInvalidateChaptersTest(volumeIndex, openChapter, chapter);
    insertRandomlyNamedBlock(volumeIndex, &testNames[index], chapter);
    testChapters[index] = chapter;
  }
}

/**********************************************************************/
static void checkForInvalidateChaptersTest(struct volume_index *volumeIndex,
                                           unsigned int numChapters,
                                           const struct uds_chunk_name *testNames,
                                           const uint64_t *testChapters,
                                           uint64_t lowChapter,
                                           uint64_t highChapter)
{
  struct volume_index_record record;
  unsigned int i;
  for (i = 0; i < numChapters; i++) {
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &testNames[i],
                                               &record));
    if ((testChapters[i] < lowChapter) || (testChapters[i] > highChapter)) {
      CU_ASSERT_FALSE(record.is_found
                      && (record.virtual_chapter == testChapters[i]));
    } else {
      CU_ASSERT_TRUE(record.is_found);
      CU_ASSERT_EQUAL(record.virtual_chapter, testChapters[i]);
    }
  }
}

/**********************************************************************/
static void rotateForInvalidateChaptersTest(struct volume_index *volumeIndex,
                                            long numChapters,
                                            struct uds_chunk_name *testNames,
                                            uint64_t *testChapters,
                                            uint64_t lowChapter,
                                            uint64_t highChapter,
                                            uint64_t *openChapter)
{
  struct volume_index_stats volumeStats;
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  long newDiscards = highChapter - *openChapter;
  long expectedDiscards = volumeStats.discard_count + newDiscards;
  uint64_t newChapter = *openChapter + 1;
  advanceForInvalidateChaptersTest(volumeIndex, openChapter, highChapter);
  checkForInvalidateChaptersTest(volumeIndex, numChapters, testNames,
                                 testChapters, lowChapter, highChapter);
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, numChapters - newDiscards);
  CU_ASSERT_EQUAL(volumeStats.discard_count, expectedDiscards);
  insertForInvalidateChaptersTest(volumeIndex, numChapters, testNames,
                                  testChapters, newChapter, highChapter,
                                  openChapter);
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, numChapters);
  CU_ASSERT_EQUAL(volumeStats.discard_count, expectedDiscards);
}

/**********************************************************************/
static void invalidateChapterTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_stats volumeStats;
  enum { CHAPTER_COUNT = SINGLE_CHAPTERS };
  struct uds_chunk_name testNames[CHAPTER_COUNT];
  uint64_t testChapters[CHAPTER_COUNT];

  // Set up the volume index to use a single delta list.
  struct configuration *config = makeTestConfig(CHAPTER_COUNT);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));

  // Deposit 1 block into each chapter.
  uint64_t openChapter = 0;
  uint64_t lowChapter = 0;
  uint64_t highChapter = CHAPTER_COUNT - 1;
  insertForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                  testChapters, lowChapter, highChapter,
                                  &openChapter);
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, CHAPTER_COUNT);
  CU_ASSERT_EQUAL(volumeStats.discard_count, 0);
  checkForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                 testChapters, lowChapter, highChapter);

  // LRU away one chapter
  lowChapter++;
  highChapter++;
  rotateForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                  testChapters, lowChapter, highChapter,
                                  &openChapter);

  // LRU away two chapters
  lowChapter  += 2;
  highChapter += 2;
  rotateForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                  testChapters, lowChapter, highChapter,
                                  &openChapter);

  // LRU away three chapters, enough times so that we wrap around twice
  while (lowChapter <= 2 * CHAPTER_COUNT) {
    lowChapter  += 3;
    highChapter += 3;
    rotateForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                    testChapters, lowChapter, highChapter,
                                    &openChapter);
  }

  // LRU away all chapters
  lowChapter  += CHAPTER_COUNT;
  highChapter += CHAPTER_COUNT;
  rotateForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                  testChapters, lowChapter, highChapter,
                                  &openChapter);

  // Rollback three chapters, as is done for restoring and replaying during
  // a restart
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  long expectedDiscards = volumeStats.discard_count + 4;
  highChapter -= 3;
  set_volume_index_open_chapter(volumeIndex, highChapter);
  openChapter = highChapter;
  checkForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                 testChapters, lowChapter, highChapter - 1);
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, CHAPTER_COUNT - 4);
  CU_ASSERT_EQUAL(volumeStats.discard_count, expectedDiscards);

  // Rollback to chapter 0, as is done for a rebuild
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  expectedDiscards = volumeStats.discard_count + volumeStats.record_count;
  set_volume_index_open_chapter(volumeIndex, 0);
  lowChapter = 0;
  highChapter = 0;
  openChapter = 0;
  checkForInvalidateChaptersTest(volumeIndex, CHAPTER_COUNT, testNames,
                                 testChapters, lowChapter, highChapter);
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, 0);
  CU_ASSERT_EQUAL(volumeStats.discard_count, expectedDiscards);

  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**
 * Test invalidating chapter with collision records.
 **/
static void invalidateChapterCollisionTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_record record;
  struct volume_index_stats volumeStats;

  // Make chunk names that use the same key
  struct uds_chunk_name name0, name1;
  createRandomBlockName(&name0);
  fillInAddress(&name0, 0);
  createRandomBlockName(&name1);
  fillInAddress(&name1, 0);

  struct configuration *config = makeTestConfig(SINGLE_CHAPTERS);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  set_volume_index_open_chapter(volumeIndex, 1);

  // Insert the first non-collision record into chapter 1
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name0, &record));
  CU_ASSERT_FALSE(record.is_found);
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, 1));

  // Insert the second collision record into chapter 0
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_TRUE(record.is_found);
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, 0));

  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, 2);
  CU_ASSERT_EQUAL(volumeStats.collision_count, 1);

  // Now invalidate chapter 0.  The collision record should disappear.
  set_volume_index_open_chapter(volumeIndex, SINGLE_CHAPTERS);

  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_FALSE(record.virtual_chapter == 0);

  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name0, &record));
  CU_ASSERT_TRUE(record.is_found);
  CU_ASSERT_TRUE(record.virtual_chapter == 1);

  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**
 * Test using the index in the presence of chapter removal.
 **/
static void rollingChaptersTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_record record;
  struct volume_index_stats volumeStats;
  const unsigned int numChapters = SINGLE_CHAPTERS;

  struct uds_chunk_name *testNames;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numChapters, struct uds_chunk_name, __func__,
                                  &testNames));

  struct configuration *config = makeTestConfig(numChapters);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));

  // Deposit 1 block into each chapter
  unsigned int i;
  for (i = 0; i < numChapters; i++) {
    set_volume_index_open_chapter(volumeIndex, i);
    insertRandomlyNamedBlock(volumeIndex, &testNames[i], i);
  }
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, numChapters);

  // Replace each block
  for (i = 0; i < numChapters; i++) {
    set_volume_index_open_chapter(volumeIndex, numChapters + i);
    insertRandomlyNamedBlock(volumeIndex, &testNames[i], numChapters + i);
    getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
    CU_ASSERT_EQUAL(volumeStats.record_count, numChapters);
  }

  // Look for each block that was just retired, then replace the block
  for (i = 0; i < numChapters; i++) {
    set_volume_index_open_chapter(volumeIndex, 2 * numChapters + i);
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &testNames[i],
                                               &record));

    CU_ASSERT_FALSE(record.is_found
                    && (record.virtual_chapter == numChapters + i));
    insertRandomlyNamedBlock(volumeIndex, &testNames[i], 2 * numChapters + i);
  }
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, numChapters);

  // Look for an existing block, then replace the retired block
  for (i = 0; i < numChapters; i++) {
    unsigned int j = i ^ 1;
    uint64_t jChapter = (j < i ? 3 : 2) * numChapters + j;
    set_volume_index_open_chapter(volumeIndex, 3 * numChapters + i);
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &testNames[j],
                                               &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(record.virtual_chapter, jChapter);
    insertRandomlyNamedBlock(volumeIndex, &testNames[i], 3 * numChapters + i);
  }
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, numChapters);

  free_volume_index(volumeIndex);
  free_configuration(config);
  UDS_FREE(testNames);
}

/**
 * Test invalidating chapter with empty delta lists.
 **/
static void invalidateChapterEmptyTest(void)
{
  struct volume_index *volumeIndex;
  struct volume_index_stats volumeStats;

  // Set up the volume index to use a single delta list and 5 chapters.
  struct configuration *config = makeTestConfig(5);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
  CU_ASSERT_EQUAL(volumeStats.record_count, 0);
  uint64_t chapter = 0;

  // Loop 3 times, looking for a problem on the 2nd and 3rd times
  unsigned int ch, loop;
  for (loop = 0; loop < 3; loop++) {

    // Insert 1 block into chapter 0 (or 5 or 10)
    struct uds_chunk_name name1;
    uint64_t chapter1 = chapter;
    set_volume_index_open_chapter(volumeIndex, chapter);
    insertRandomlyNamedBlock(volumeIndex, &name1, chapter1);
    getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
    CU_ASSERT_EQUAL(volumeStats.record_count, 1);

    // Advance 4 chapters
    for (ch = 0; ch < 4; ch++) {
      set_volume_index_open_chapter(volumeIndex, ++chapter);
      getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
      CU_ASSERT_EQUAL(volumeStats.record_count, 1);
    }

    // The block should still be there
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(record.virtual_chapter, chapter1);

    // Advance 1 chapter.  The block should disappear when we look for it.
    set_volume_index_open_chapter(volumeIndex, ++chapter);
    getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
    CU_ASSERT_EQUAL(volumeStats.record_count, 1);
    UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name1, &record));
    CU_ASSERT_FALSE(record.is_found);
    getVolumeIndexStatsDenseOnly(volumeIndex, &volumeStats);
    CU_ASSERT_EQUAL(volumeStats.record_count, 0);
  }

  free_volume_index(volumeIndex);
  free_configuration(config);
}

/**********************************************************************/

static const CU_TestInfo volumeIndexTests[] = {
  {"Initialization",                initializationTest },
  {"Basic",                         basicTest },
  {"Set chapter",                   setChapterTest },
  {"Invalidate 123",                invalidate123Test },
  {"Invalidate 132",                invalidate132Test },
  {"Invalidate 213",                invalidate213Test },
  {"Invalidate 231",                invalidate231Test },
  {"Invalidate 312",                invalidate312Test },
  {"Invalidate 321",                invalidate321Test },
  {"Invalidate chapter",            invalidateChapterTest },
  {"Invalidate chapters collision", invalidateChapterCollisionTest },
  {"Invalidate chapters empty",     invalidateChapterEmptyTest },
  {"Rolling chapters",              rollingChaptersTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "VolumeIndex_t1",
  .initializer = volumeIndexInit,
  .cleaner     = volumeIndexCleanup,
  .tests       = volumeIndexTests,
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
