// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * VolumeIndexSave_p1 measures the time to save and restore a volume index.
 **/

#include "albtest.h"
#include "assertions.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "time-utils.h"

static struct configuration *config;

/**
 * Insert a randomly named block
 **/
static void insertRandomlyNamedBlock(struct volume_index *volumeIndex,
                                     uint64_t virtualChapter)
{
  static uint64_t nameCounter = 0;
  struct uds_chunk_name name
    = murmurHashChunkName(&nameCounter, sizeof(nameCounter), 0);
  nameCounter += 1;

  struct volume_index_record record;
  UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name, &record));
  UDS_ASSERT_SUCCESS(put_volume_index_record(&record, virtualChapter));
}

/**********************************************************************/
static void reportIOTime(const char *title, ktime_t elapsed)
{
  char *elapsedTime;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&elapsedTime, elapsed, 0));
  albPrint("%s elapsed time %s", title, elapsedTime);
  UDS_FREE(elapsedTime);
}

/**********************************************************************/
static void reportTimes(const char *title, long numBlocks, ktime_t elapsed)
{
  char *elapsedTime, *perRecord;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&elapsedTime, elapsed, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&perRecord, elapsed, numBlocks));
  albPrint("%s %ld blocks took %s, average = %s/record",
           title, numBlocks, elapsedTime, perRecord);
  UDS_FREE(elapsedTime);
  UDS_FREE(perRecord);
}

/**********************************************************************/
static void reportVolumeIndexMemory(struct volume_index *volumeIndex)
{
  struct volume_index_stats combinedStats;
  get_volume_index_combined_stats(volumeIndex, &combinedStats);

  long numBlocks = combinedStats.record_count;
  long numLists = combinedStats.num_lists;
  size_t memAlloc = combinedStats.memory_allocated;
  size_t memUsed = get_volume_index_memory_used(volumeIndex);
  if (numBlocks == 0) {
    albPrint("Memory: allocated %zd bytes for %ld delta lists (%zd each)",
             memAlloc, numLists, memAlloc / numLists);
  } else {
    albPrint("Memory: used %zd bytes in %ld delta lists (%zd each)",
             memUsed, numLists, memUsed / numLists);
  }
  albFlush();
}

/**********************************************************************/
static ktime_t fillChapter(struct volume_index *mi, uint64_t virtualChapter)
{
  int blocksPerChapter = config->geometry->records_per_chapter;
  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
  set_volume_index_open_chapter(mi, virtualChapter);
  int count;
  for (count = 0; count < blocksPerChapter; count++) {
    insertRandomlyNamedBlock(mi, virtualChapter);
  }
  return ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
}

/**********************************************************************/
static void fillTestIndex(struct volume_index *volumeIndex)
{
  /*
   * We report progress after every 4M chunks.  This interval cannot be larger
   * than the number of chunks that can be posted in 22 seconds.  If it is too
   * large, then running this test in the kernel will report soft lockups.
   */
  enum { REPORT_INTERVAL = 1 << 22 };

  // Fill the index, reporting after every 4M chunks
  ktime_t elapsed = 0;
  int blocksPerChapter = config->geometry->records_per_chapter;
  int chapterCount = config->geometry->chapters_per_volume;
  int fillGroupMask = (REPORT_INTERVAL / blocksPerChapter) - 1;
  long numBlocks = 0;
  int chapter;
  albPrint("reporting every %d chapters", fillGroupMask + 1);
  for (chapter = 0; chapter < chapterCount; chapter++) {
    ktime_t chapterElapsed = fillChapter(volumeIndex, chapter);
    elapsed += chapterElapsed;
    numBlocks += blocksPerChapter;
    if ((chapter & fillGroupMask) == fillGroupMask) {
      reportTimes("Last:  ", blocksPerChapter, chapterElapsed);
      reportTimes("Total: ", numBlocks, elapsed);
      albFlush();
    }
  }
}

/**********************************************************************/
static void saveTestIndex(struct volume_index *volumeIndex,
                          struct io_factory *factory,
                          size_t saveSize)
{
  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, 0, saveSize, &writer));
  UDS_ASSERT_SUCCESS(save_volume_index(volumeIndex, &writer, 1));
  free_buffered_writer(writer);

  ktime_t saveTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
  reportIOTime("saveVolumeIndex:", saveTime);
}

/**********************************************************************/
static struct volume_index *restoreTestIndex(struct io_factory *factory,
                                             size_t saveSize)
{
  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
  struct volume_index *volumeIndex;
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, saveSize, &reader));
  put_uds_io_factory(factory);
  UDS_ASSERT_SUCCESS(load_volume_index(volumeIndex, &reader, 1));
  free_buffered_reader(reader);
  ktime_t restoreTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
  reportIOTime("load_volume_index():", restoreTime);
  return volumeIndex;
}

/**********************************************************************/
static void saveRestoreTest(void)
{
  struct volume_index *volumeIndex;
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  reportVolumeIndexMemory(volumeIndex);

  fillTestIndex(volumeIndex);
  reportVolumeIndexMemory(volumeIndex);

  // Capture statistics for the initial index
  struct volume_index_stats denseStats1, sparseStats1;
  get_volume_index_stats(volumeIndex, &denseStats1, &sparseStats1);
  size_t used1 = get_volume_index_memory_used(volumeIndex);

  uint64_t blockCount;
  UDS_ASSERT_SUCCESS(compute_volume_index_save_blocks(config, UDS_BLOCK_SIZE,
                                                      &blockCount));
  size_t saveSize = blockCount * UDS_BLOCK_SIZE;
  struct io_factory *factory = getTestIOFactory();
  saveTestIndex(volumeIndex, factory, saveSize);
  free_volume_index(volumeIndex);

  volumeIndex = restoreTestIndex(factory, saveSize);
  reportVolumeIndexMemory(volumeIndex);

  // Compare restored index to the initial index
  struct volume_index_stats denseStats2, sparseStats2;
  get_volume_index_stats(volumeIndex, &denseStats2, &sparseStats2);
  CU_ASSERT(get_volume_index_memory_used(volumeIndex) <= used1);
  CU_ASSERT_EQUAL(denseStats1.record_count, denseStats2.record_count);
  CU_ASSERT_EQUAL(sparseStats1.record_count, sparseStats2.record_count);

  free_volume_index(volumeIndex);
}

/**********************************************************************/
static void initSuite(int argc, const char **argv)
{
  config = createConfigForAlbtest(argc, argv);
  config->zone_count = 1;
}

/**********************************************************************/
static void cleanSuite(void)
{
  free_configuration(config);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "save restore performance", saveRestoreTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "VolumeIndexSave_p1",
  .initializerWithArguments = initSuite,
  .cleaner                  = cleanSuite,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
