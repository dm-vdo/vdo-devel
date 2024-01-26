// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * VolumeIndex_p1 measures the single-threaded single zone performance of
 * the volume index.  It times the filling phase and steady state
 * operation.
 **/

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static struct uds_configuration *config;

// These counters count collisions encountered while inserting blocks.
static unsigned long denseCollisions  = 0;
static unsigned long sparseCollisions = 0;

/**
 * Insert a randomly named block
 **/
static void insertRandomlyNamedBlock(struct volume_index *volumeIndex,
                                     uint64_t virtualChapter)
{
  static uint64_t nameCounter = 0;
  struct uds_record_name name
    = hash_record_name(&nameCounter, sizeof(nameCounter));
  nameCounter += 1;

  struct volume_index_record record;
  UDS_ASSERT_SUCCESS(uds_get_volume_index_record(volumeIndex, &name, &record));
  if (record.is_found) {
    if (uds_is_volume_index_sample(volumeIndex, &name)) {
      sparseCollisions++;
    } else {
      denseCollisions++;
    }
  }
  UDS_ASSERT_SUCCESS(uds_put_volume_index_record(&record, virtualChapter));
}

/**********************************************************************/
static void reportTimes(const char *title, long numBlocks, ktime_t elapsed)
{
  char *total, *perRecord;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&total, elapsed));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&perRecord, elapsed / numBlocks));
  albPrint("%s %ld blocks took %s, average = %s/record",
           title, numBlocks, total, perRecord);
  uds_free(total);
  uds_free(perRecord);
}

/**********************************************************************/
static void reportRebalances(u32 *rebalanceCount, const char *label,
                             const struct volume_index_stats *mis)
{
  if (*rebalanceCount != mis->rebalance_count) {
    *rebalanceCount = mis->rebalance_count;
    char *rebalanceTime;
    UDS_ASSERT_SUCCESS(rel_time_to_string(&rebalanceTime,
                                          mis->rebalance_time));
    albPrint("%s: %d rebalances in %s", label, mis->rebalance_count,
             rebalanceTime);
    uds_free(rebalanceTime);
  }
}

/**********************************************************************/
static void reportIndexMemoryUsage(struct volume_index *volumeIndex)
{
  struct volume_index_stats denseStats, sparseStats;
  get_volume_index_separate_stats(volumeIndex, &denseStats, &sparseStats);

  long numBlocks = denseStats.record_count + sparseStats.record_count;
  size_t memAlloc = volumeIndex->memory_size;
  size_t memUsed = get_volume_index_memory_used(volumeIndex);
  int usedBytesPerRecord  = 100 * memUsed / numBlocks;
  int allocBytesPerRecord = 100 * memAlloc / numBlocks;
  albPrint("Memory: allocated = %zd MBytes (%d.%02d bytes/record),"
           " used = %zd MBytes (%d.%02d bytes/record)",
           memAlloc / MEGABYTE,
           allocBytesPerRecord / 100, allocBytesPerRecord % 100,
           memUsed  / MEGABYTE,
           usedBytesPerRecord / 100, usedBytesPerRecord % 100);

  static u32 denseRebalanceCount = 0;
  reportRebalances(&denseRebalanceCount, "Dense", &denseStats);
  static u32 sparseRebalanceCount = 0;
  reportRebalances(&sparseRebalanceCount, "Sparse", &sparseStats);
}

/**********************************************************************/
static void reportCollisions(struct volume_index *volumeIndex)
{
  struct volume_index_stats denseStats, sparseStats;
  get_volume_index_separate_stats(volumeIndex, &denseStats, &sparseStats);
  long numCollisions = denseStats.collision_count + sparseStats.collision_count;
  long numBlocks = denseStats.record_count + sparseStats.record_count;
  int collRate = 1000000 * numCollisions / numBlocks;
  albPrint("%ld blocks with %ld collisions (0.%06d)", numBlocks, numCollisions, collRate);
  if ((denseStats.record_count > 0) && (sparseStats.record_count > 0)) {
    collRate = 1000000 * denseStats.collision_count / denseStats.record_count;
    albPrint("%llu dense blocks with %llu collisions (0.%06d)",
             (unsigned long long) denseStats.record_count,
             (unsigned long long) denseStats.collision_count, collRate);
    collRate = 1000000 * sparseStats.collision_count / sparseStats.record_count;
    albPrint("%llu sparse blocks with %llu collisions (0.%06d)",
             (unsigned long long) sparseStats.record_count,
             (unsigned long long) sparseStats.collision_count, collRate);
  }
}

/**********************************************************************/
static ktime_t fillChapter(struct volume_index *mi, uint64_t virtualChapter)
{
  int blocksPerChapter = config->geometry->records_per_chapter;
  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  uds_set_volume_index_open_chapter(mi, virtualChapter);
  int count;
  for (count = 0; count < blocksPerChapter; count++) {
    insertRandomlyNamedBlock(mi, virtualChapter);
  }
  return ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
}

/**********************************************************************/
static void miPerfTest(void)
{
  int blocksPerChapter = config->geometry->records_per_chapter;
  int chapterCount = config->geometry->chapters_per_volume;

  struct volume_index *volumeIndex;
  UDS_ASSERT_SUCCESS(uds_make_volume_index(config, 0, &volumeIndex));

  struct volume_index_stats stats;
  uds_get_volume_index_stats(volumeIndex, &stats);
  int listCount = stats.delta_lists;
  size_t memAlloc = volumeIndex->memory_size;
  albPrint("Initial Memory: allocated %zd for %d delta lists (%zd each)",
           memAlloc, listCount, memAlloc / listCount);
  albFlush();

  // Fill the index, reporting after every 4M chunks
  int fillGroupMask = ((1 << 22) / blocksPerChapter) - 1;
  ktime_t elapsed = 0;
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
      reportIndexMemoryUsage(volumeIndex);
      albFlush();
    }
  }
  reportCollisions(volumeIndex);

  // We want to process 64M chunks in steady state
  int steadyStateChapterCount = (1 << 26) / blocksPerChapter;
  // Report after every 2M chunks
  int steadyGroupMask = fillGroupMask >> 1;
  elapsed = 0;
  numBlocks = 0;
  denseCollisions = 0;
  sparseCollisions = 0;
  albPrint("reporting every %d chapters", steadyGroupMask + 1);
  for (chapter = 0; chapter < steadyStateChapterCount; chapter++) {
    ktime_t chapterElapsed = fillChapter(volumeIndex, chapterCount + chapter);
    elapsed += chapterElapsed;
    numBlocks += blocksPerChapter;

    if ((chapter & steadyGroupMask) == steadyGroupMask) {
      reportTimes("Steady:  ", numBlocks, elapsed);
      reportIndexMemoryUsage(volumeIndex);
      albFlush();
    }
  }
  reportCollisions(volumeIndex);
  if (sparseCollisions > 0) {
    albPrint("In %ld insertions, there were %lu dense collisions and %lu sparse"
             " collisions", numBlocks, denseCollisions, sparseCollisions);
  }

  uds_free_volume_index(volumeIndex);
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
  uds_free_configuration(config);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "volume index performance", miPerfTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "VolumeIndex_p1",
  .initializerWithArguments = initSuite,
  .cleaner                  = cleanSuite,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
