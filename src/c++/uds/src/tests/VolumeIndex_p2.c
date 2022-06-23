// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * VolumeIndex_p2 measures the multi-threaded and multizone performance of
 * the volume index.  It measures the steady state performance and tests
 * that adding zones (with 1 thread per zone) improves performance until we
 * run out of CPU cores.
 **/

#include <math.h>

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "testPrototypes.h"

static struct configuration *config;
static struct geometry      *geometry;
static struct io_factory    *factory;
static struct volume_index  *volumeIndex;
static size_t                zoneSize;

/**
 * This counter is hashed to generate a fixed sequence of chunk names, which
 * produces the exact same number of collisions for each test run.
 **/
static uint64_t nameCounter = 0;

/**********************************************************************/
static unsigned int chunksSeen(const struct volume_index_stats *denseStats,
                               const struct volume_index_stats *sparseStats)
{
  return (denseStats->record_count
          + denseStats->discard_count
          + denseStats->overflow_count
          + sparseStats->record_count
          + sparseStats->discard_count
          + sparseStats->overflow_count);
}

/**********************************************************************/
static void reportCollisions(const struct volume_index_stats *denseStats,
                             const struct volume_index_stats *sparseStats)
{
  long collisions = denseStats->collision_count + sparseStats->collision_count;
  long numBlocks  = denseStats->record_count    + sparseStats->record_count;
  albPrint("%ld blocks with %ld collisions (%f)", numBlocks, collisions,
           (double) collisions / numBlocks);
  if ((denseStats->record_count > 0) && (sparseStats->record_count > 0)) {
    albPrint("%ld dense blocks with %ld collisions (%f)",
             denseStats->record_count, denseStats->collision_count,
             (double) denseStats->collision_count / denseStats->record_count);
    albPrint("%ld sparse blocks with %ld collisions (%f)",
             sparseStats->record_count, sparseStats->collision_count,
             (double) sparseStats->collision_count
                        / sparseStats->record_count);
  }
}

/**********************************************************************/
static void reportRebalances(const char *label,
                             const struct volume_index_stats *mis)
{
  char *rebalanceTime;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&rebalanceTime,
                                        mis->rebalance_time, 0));
  albPrint("%d %s rebalances in %s", mis->rebalance_count, label,
           rebalanceTime);
  UDS_FREE(rebalanceTime);
}

/**********************************************************************/
static void
reportIndexMemoryUsage(const struct volume_index_stats *denseStats,
                       const struct volume_index_stats *sparseStats)
{
  long numBlocks = denseStats->record_count + sparseStats->record_count;
  size_t memAlloc
    = denseStats->memory_allocated + sparseStats->memory_allocated;
  size_t memUsed = get_volume_index_memory_used(volumeIndex);
  double usedBytesPerRecord  = (double)memUsed / (double)numBlocks;
  double allocBytesPerRecord = (double)memAlloc / (double)numBlocks;
  albPrint("Memory: allocated = %.1f MBytes (%.2f bytes/record),"
           " used = %.1f MBytes (%.2f bytes/record)",
           (double) memAlloc / MEGABYTE, allocBytesPerRecord,
           (double) memUsed  / MEGABYTE, usedBytesPerRecord);

  if (sparseStats->record_count > 0) {
    reportRebalances("dense", denseStats);
    reportRebalances("sparse", sparseStats);
  } else {
    reportRebalances("all", denseStats);
  }
}

/**********************************************************************/
static void reportTimes(const char *title, unsigned int numZones,
                        unsigned long numBlocks, ktime_t elapsed)
{
  char *total, *perRecord;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&total, elapsed, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&perRecord, elapsed, numBlocks));
  albPrint("%s %u zones %lu blocks took %s, average = %s/record",
           title, numZones, numBlocks, total, perRecord);
  UDS_FREE(total);
  UDS_FREE(perRecord);
}

/**********************************************************************/
typedef struct threadAdder {
  unsigned long  count;
  unsigned int   zone;
  struct thread *thread;
} ThreadAdder;

/**********************************************************************/
static void threadAdd(void *arg)
{
  ThreadAdder *ta = (ThreadAdder *) arg;
  for (unsigned long i = 0; i < ta->count; i++) {
    uint64_t counter = nameCounter + i;
    uint64_t chapter = counter / geometry->records_per_chapter;
    if (counter % geometry->records_per_chapter == 0) {
      set_volume_index_zone_open_chapter(volumeIndex, ta->zone, chapter);
    }
    struct uds_chunk_name name
      = murmurHashChunkName(&counter, sizeof(counter), 0);
    if (get_volume_index_zone(volumeIndex, &name) == ta->zone) {
      struct volume_index_record record;
      UDS_ASSERT_SUCCESS(get_volume_index_record(volumeIndex, &name, &record));
      UDS_ASSERT_SUCCESS(put_volume_index_record(&record, chapter));
    }
  }
}

/**********************************************************************/
static void createAndFill(unsigned int numZones)
{
  config->zone_count = numZones;
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));

  unsigned long chunkCount = (geometry->records_per_chapter
                              * (geometry->chapters_per_volume + 64));

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  ThreadAdder ta[numZones];
  for (unsigned int z = 0; z < numZones; z++) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(NULL, nameBuf, sizeof(nameBuf),
                                         UDS_INVALID_ARGUMENT,
                                         "adder%d", z));
    ta[z].count = chunkCount;
    ta[z].zone = z;
    UDS_ASSERT_SUCCESS(uds_create_thread(threadAdd, &ta[z], nameBuf,
                                         &ta[z].thread));
  }
  for (unsigned int z = 0; z < numZones; z++) {
    UDS_ASSERT_SUCCESS(uds_join_threads(ta[z].thread));
  }
  ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
  nameCounter += chunkCount;

  reportTimes("Fill", numZones, chunkCount, elapsed);
  struct volume_index_stats denseStats, sparseStats;
  get_volume_index_stats(volumeIndex, &denseStats, &sparseStats);
  reportIndexMemoryUsage(&denseStats, &sparseStats);
  reportCollisions(&denseStats, &sparseStats);
  fflush(stdout);
}

/**********************************************************************/
static ktime_t steady(unsigned int numZones)
{
  unsigned long chunkCount = 64 << 20;

  // Compute the number of chunks that the volume index has seen
  struct volume_index_stats denseStats, sparseStats;
  get_volume_index_stats(volumeIndex, &denseStats, &sparseStats);
  unsigned long chunksBefore = chunksSeen(&denseStats, &sparseStats);

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  ThreadAdder ta[numZones];
  for (unsigned int z = 0; z < numZones; z++) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(NULL, nameBuf, sizeof(nameBuf),
                                         UDS_INVALID_ARGUMENT,
                                         "adder%d", z));
    ta[z].count = chunkCount;
    ta[z].zone = z;
    UDS_ASSERT_SUCCESS(uds_create_thread(threadAdd, &ta[z], nameBuf,
                                         &ta[z].thread));
  }
  for (unsigned int z = 0; z < numZones; z++) {
    UDS_ASSERT_SUCCESS(uds_join_threads(ta[z].thread));
  }
  ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);

  reportTimes("Steady", numZones, chunkCount, elapsed);
  get_volume_index_stats(volumeIndex, &denseStats, &sparseStats);
  reportIndexMemoryUsage(&denseStats, &sparseStats);
  reportCollisions(&denseStats, &sparseStats);

  // Make sure the volume index has now seen the proper number of chunks
  unsigned long chunksAfter = chunksSeen(&denseStats, &sparseStats);
  CU_ASSERT_EQUAL(chunkCount, chunksAfter - chunksBefore);

  fflush(stdout);
  return elapsed;
}

/**********************************************************************/
static void save(unsigned int numZones)
{
  uint64_t saveBlockCount;
  UDS_ASSERT_SUCCESS(compute_volume_index_save_blocks(config, UDS_BLOCK_SIZE,
                                                      &saveBlockCount));
  uint64_t zoneBlockCount = (saveBlockCount + numZones - 1) / numZones;
  zoneSize = zoneBlockCount * UDS_BLOCK_SIZE;

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  struct buffered_writer *writers[numZones];
  for (unsigned int z = 0; z < numZones; z++) {
    UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, z * zoneSize,
                                                zoneSize, &writers[z]));
  }

  UDS_ASSERT_SUCCESS(save_volume_index(volumeIndex, writers, numZones));
  for (unsigned int z = 0; z < numZones; z++) {
    free_buffered_writer(writers[z]);
  }

  ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
  char *total;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&total, elapsed, 0));
  albPrint("Saved %u zones in %s", numZones, total);
  UDS_FREE(total);
}

/**********************************************************************/
static void restore(unsigned int oldZones, unsigned int newZones)
{
  free_volume_index(volumeIndex);
  config->zone_count = newZones;

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  UDS_ASSERT_SUCCESS(make_volume_index(config, 0, &volumeIndex));
  struct buffered_reader *readers[oldZones];
  for (unsigned int z = 0; z < oldZones; z++) {
    UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, z * zoneSize,
                                                zoneSize, &readers[z]));
  }
  UDS_ASSERT_SUCCESS(load_volume_index(volumeIndex, readers, oldZones));
  for (unsigned int z = 0; z < oldZones; z++) {
    free_buffered_reader(readers[z]);
  }
  ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
  char *total;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&total, elapsed, 0));
  albPrint("Restored %u zones in %s", oldZones, total);
  UDS_FREE(total);
}

/**********************************************************************/
static void miPerfTest(void)
{
  unsigned int numCores = uds_get_num_cores();
  unsigned int defaultZones = config->zone_count;
  createAndFill(defaultZones);
  save(defaultZones);

  // Loop over differing numbers of zones
  unsigned int maxZones = defaultZones + 2;
  if (maxZones > MAX_ZONES) {
    maxZones = MAX_ZONES;
  }
  double steadyTimes[maxZones + 1];
  for (unsigned int z = maxZones; z > 0; z--) {
    // Restore the saved state, changing the number of zones
    restore(defaultZones, z);
    // Run the steady state test using the loop's number of zones
    steadyTimes[z] = steady(z) / 1.0e9;
  }
  free_volume_index(volumeIndex);

  /*
   * Expect nearly linear speedup until we run out of cores.
   *
   * Real data from porter-64 (4 cores) on 29-Aug-11:
   *    2 cores -  2.06% different
   *    3 cores -  4.74% different
   *    4 cores - 12.04% different
   */
  for (unsigned int z = 2; (z <= numCores) && (z <= maxZones); z++) {
    // Compute how close we come to N zones being N times faster.
    double relativeSpeed = steadyTimes[1] / (z * steadyTimes[z]);
    albPrint("For %u zones, relative speed is %f compared to 1 zone",
             z, relativeSpeed);
    //  Accept a performance difference of up to 5% plus 2% per zone.
    CU_ASSERT(fabs(relativeSpeed - 1.0) < 0.05 + 0.02 * z);
  }

  /*
   * Expect the total time to remain steady when we oversubscribe the cores.
   * Accept a performance drop of up to 25%.  If there really are more cores
   * than uds_get_num_cores() returns, performance can keep getting better.
   */
  for (unsigned int z = numCores + 1; z <= maxZones; z++) {
    double relativeSpeed = steadyTimes[numCores] / steadyTimes[z];
    albPrint("For %u zones, relative speed is %f compared to %u zones",
             z, relativeSpeed, numCores);
    CU_ASSERT(relativeSpeed > 1.0 - 0.25);
  }
}

/**********************************************************************/
static void initSuite(int argc, const char **argv)
{
  config = createConfigForAlbtest(argc, argv);
  geometry = config->geometry;
  factory = getTestIOFactory();
}

/**********************************************************************/
static void cleanSuite(void)
{
  free_configuration(config);
  put_uds_io_factory(factory);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "volume index performance", miPerfTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "VolumeIndex_p2",
  .initializerWithArguments = initSuite,
  .cleaner                  = cleanSuite,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
