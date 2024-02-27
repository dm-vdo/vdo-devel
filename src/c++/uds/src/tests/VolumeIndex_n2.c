// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "volume-index.h"

enum { ZONES = 5 };

typedef struct testMI {
                                             // Test state:
  struct volume_index       *mi;             //     volume index
  struct uds_configuration   config;         //     configuration
  struct index_geometry      geometry;       //     geometry
  long                       entryCounter;   // number of records written
  unsigned int               numZones;       // number of zones
  struct io_factory         *factory;        // IO factory for saving volume
                                             // index
                                             // Save state:
  off_t                      zoneOff[ZONES]; //     save area offset
  size_t                     saveSize;       //     size of memory IOregion
  struct volume_index_stats  denseStats;     //     dense index stats
  struct volume_index_stats  sparseStats;    //     sparse index stats
  size_t                     memoryUsed;     //     memory used
  bool                       statsValid;     //     stats are valid
} TestMI;

typedef struct threadMI {
  TestMI        *testmi;   // Test volume index
  unsigned int   zone;     // Thread zone number
  struct thread *thread;   // Thread handle
} ThreadMI;

static struct block_device *testDevice;

/**********************************************************************/
static TestMI *openVolumeIndex(unsigned int numZones, bool sparse)
{
  TestMI *testmi;
  UDS_ASSERT_SUCCESS(vdo_allocate(1, TestMI, __func__, &testmi));
  testmi->numZones = numZones;

  // Make the test geometry
  testmi->geometry.chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME;
  testmi->geometry.records_per_chapter = DEFAULT_RECORDS_PER_PAGE;

  // Make the test configuration
  testmi->config.geometry = &testmi->geometry;
  testmi->config.volume_index_mean_delta = DEFAULT_VOLUME_INDEX_MEAN_DELTA;
  testmi->config.zone_count = numZones;

  if (sparse) {
    testmi->geometry.chapters_per_volume = 10 * DEFAULT_CHAPTERS_PER_VOLUME;
    testmi->geometry.sparse_chapters_per_volume
      = testmi->geometry.chapters_per_volume - DEFAULT_CHAPTERS_PER_VOLUME / 2;
    testmi->config.sparse_sample_rate = 32;
  }

  // Create the volume index
  UDS_ASSERT_SUCCESS(uds_make_volume_index(&testmi->config, 0, &testmi->mi));

  // Compute the volume index saved byte stream size
  uint64_t blockCount;
  UDS_ASSERT_SUCCESS(uds_compute_volume_index_save_blocks(&testmi->config,
                                                          UDS_BLOCK_SIZE,
                                                          &blockCount));
  testmi->saveSize = blockCount * UDS_BLOCK_SIZE;

  // Set the starting point for each save zone
  unsigned int z;
  for (z = 0; z < ZONES; z++) {
    testmi->zoneOff[z] = z * testmi->saveSize;
  }

  testDevice = getTestBlockDevice();
  UDS_ASSERT_SUCCESS(uds_make_io_factory(testDevice, &testmi->factory));
  return testmi;
}

/**********************************************************************/
static void saveVolumeIndex(TestMI *testmi)
{
  unsigned int z;
  struct buffered_writer *writers[ZONES];

  for (z = 0; z < testmi->numZones; z++) {
    UDS_ASSERT_SUCCESS(uds_make_buffered_writer(testmi->factory,
                                                testmi->zoneOff[z],
                                                testmi->saveSize,
                                                &writers[z]));
  }

  UDS_ASSERT_SUCCESS(uds_save_volume_index(testmi->mi, writers, testmi->numZones));
  for (z = 0; z < testmi->numZones; z++) {
    uds_free_buffered_writer(writers[z]);
  }

  get_volume_index_separate_stats(testmi->mi, &testmi->denseStats, &testmi->sparseStats);
  testmi->memoryUsed = get_volume_index_memory_used(testmi->mi);
  testmi->statsValid = true;
}

/**********************************************************************/
static void reopenVolumeIndex(TestMI       *testmi,
                              unsigned int  numZones,
                              int           status)
{
  uds_free_volume_index(testmi->mi);
  testmi->mi = NULL;

  testmi->config.zone_count = numZones;
  UDS_ASSERT_SUCCESS(uds_make_volume_index(&testmi->config, 0, &testmi->mi));

  struct buffered_reader *readers[ZONES];
  unsigned int z;
  for (z = 0; z < testmi->numZones; z++) {
    UDS_ASSERT_SUCCESS(uds_make_buffered_reader(testmi->factory,
                                                testmi->zoneOff[z],
                                                testmi->saveSize,
                                                &readers[z]));
  }
  UDS_ASSERT_ERROR(status, uds_load_volume_index(testmi->mi, readers, testmi->numZones));
  for (z = 0; z < testmi->numZones; z++) {
    uds_free_buffered_reader(readers[z]);
  }

  if ((status == UDS_SUCCESS) && testmi->statsValid) {
    struct volume_index_stats denseStats, sparseStats;
    get_volume_index_separate_stats(testmi->mi, &denseStats, &sparseStats);
    CU_ASSERT_EQUAL(testmi->denseStats.record_count, denseStats.record_count);
    CU_ASSERT_EQUAL(testmi->denseStats.collision_count, denseStats.collision_count);
    CU_ASSERT_EQUAL(testmi->sparseStats.record_count, sparseStats.record_count);
    CU_ASSERT_EQUAL(testmi->sparseStats.collision_count, sparseStats.collision_count);
    CU_ASSERT_EQUAL(testmi->memoryUsed, get_volume_index_memory_used(testmi->mi));
  }

  testmi->numZones = numZones;
}

/**********************************************************************/
static void addToVolumeIndex(TestMI *testmi, int count)
{
  struct volume_index_stats stats;
  uds_get_volume_index_stats(testmi->mi, &stats);
  CU_ASSERT_EQUAL(stats.record_count, testmi->entryCounter);
  int i;
  for (i = 0; i < count; i++) {
    uint64_t counter = testmi->entryCounter++;
    uint64_t chapter = counter / testmi->geometry.records_per_chapter;
    if (counter % testmi->geometry.records_per_chapter == 0) {
      uds_set_volume_index_open_chapter(testmi->mi, chapter);
    }
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
    UDS_ASSERT_SUCCESS(uds_put_volume_index_record(&record, chapter));
  }
  uds_get_volume_index_stats(testmi->mi, &stats);
  CU_ASSERT_EQUAL(stats.record_count, testmi->entryCounter);
}

/**********************************************************************/
static void verifyVolumeIndex(TestMI *testmi)
{
  struct volume_index_stats stats;
  uds_get_volume_index_stats(testmi->mi, &stats);
  CU_ASSERT_EQUAL(stats.record_count, testmi->entryCounter);
  long i;
  for (i = 0; i < testmi->entryCounter; i++) {
    uint64_t counter = i;
    uint64_t chapter = counter / testmi->geometry.records_per_chapter;
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
    CU_ASSERT_TRUE(record.is_found);
    CU_ASSERT_EQUAL(record.virtual_chapter, chapter);
    uint64_t virtual_chapter = uds_lookup_volume_index_name(testmi->mi, &name);
    if (uds_is_volume_index_sample(testmi->mi, &name)) {
      CU_ASSERT_EQUAL(virtual_chapter, chapter);
    } else {
      CU_ASSERT_EQUAL(virtual_chapter, NO_CHAPTER);
    }
  }
}

/**********************************************************************/
static void overflowVolumeIndex(TestMI *testmi)
{
  uint64_t extraCounter = 0;
  struct volume_index_stats stats;
  uds_get_volume_index_stats(testmi->mi, &stats);
  CU_ASSERT_EQUAL(stats.early_flushes, 0);
  for (;;) {
    uint64_t counter = testmi->entryCounter++;
    uint64_t chapter = counter / testmi->geometry.records_per_chapter;
    if (counter % testmi->geometry.records_per_chapter == 0) {
      uds_set_volume_index_open_chapter(testmi->mi, chapter);
      uds_get_volume_index_stats(testmi->mi, &stats);
      if (stats.early_flushes > 0) {
        break;
      }
    }
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
    UDS_ASSERT_SUCCESS(uds_put_volume_index_record(&record, chapter));
    if (counter % 8 == 0) {
      counter = --extraCounter;
      name = hash_record_name(&counter, sizeof(counter));
      UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
      UDS_ASSERT_SUCCESS(uds_put_volume_index_record(&record, chapter));
    }
  }
}

/**********************************************************************/
static void closeVolumeIndex(TestMI *testmi)
{
  uds_free_volume_index(testmi->mi);
  uds_put_io_factory(testmi->factory);
  putTestBlockDevice(testDevice);
  vdo_free(testmi);
}

/**********************************************************************/
static void threadAddToVolumeIndex(TestMI *testmi, unsigned int zoneNumber,
                                   long entryCounter, int count)
{
  int i;
  for (i = 0; i < count; i++) {
    uint64_t counter = entryCounter + i;
    uint64_t chapter = counter / testmi->geometry.records_per_chapter;
    if (counter % testmi->geometry.records_per_chapter == 0) {
      uds_set_volume_index_zone_open_chapter(testmi->mi, zoneNumber, chapter);
    }
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
    if (uds_get_volume_index_zone(testmi->mi, &name) == zoneNumber) {
      struct volume_index_record record;
      UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
      UDS_ASSERT_SUCCESS(uds_put_volume_index_record(&record, chapter));
    }
  }
}

/**********************************************************************/
static void threadVerifyVolumeIndex(TestMI *testmi, unsigned int zoneNumber,
                                    long entryCounter)
{
  long i;
  for (i = 0; i < entryCounter; i++) {
    uint64_t counter = i;
    uint64_t chapter = counter / testmi->geometry.records_per_chapter;
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
      
    if (uds_get_volume_index_zone(testmi->mi, &name) == zoneNumber) {
      struct volume_index_record record;
      UDS_ASSERT_SUCCESS(uds_get_volume_index_record(testmi->mi, &name, &record));
      CU_ASSERT_TRUE(record.is_found);
      CU_ASSERT_EQUAL(record.virtual_chapter, chapter);
    }
  }
}

/**********************************************************************/
static void testMostlyEmpty(unsigned int numZones, bool sparse)
{
  TestMI *testmi = openVolumeIndex(numZones, sparse);

  // Save and restore an empty volume index
  saveVolumeIndex(testmi);
  reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);

  // Save and restore a volume index with up to 2x4 entries
  int i;
  for (i = 0; i < 4; i++) {
    addToVolumeIndex(testmi, 1);
    saveVolumeIndex(testmi);
    reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);
    verifyVolumeIndex(testmi);

    addToVolumeIndex(testmi, 1);
    saveVolumeIndex(testmi);
    verifyVolumeIndex(testmi);
    reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);
    verifyVolumeIndex(testmi);
  }

  closeVolumeIndex(testmi);
}

/**********************************************************************/
static void testChangingZones(unsigned int numZones, bool sparse)
{
  enum { REC_COUNT = 1331 };
  TestMI *testmi = openVolumeIndex(numZones, sparse);

  unsigned int z;
  for (z = 1; z < ZONES; z++) {
    if (z != numZones) {
      // Switch to an alternate number of zones
      addToVolumeIndex(testmi, REC_COUNT);
      saveVolumeIndex(testmi);
      verifyVolumeIndex(testmi);
      reopenVolumeIndex(testmi, z, UDS_SUCCESS);
      verifyVolumeIndex(testmi);

      // Switch back to the starting number of zones
      addToVolumeIndex(testmi, REC_COUNT);
      saveVolumeIndex(testmi);
      verifyVolumeIndex(testmi);
      reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);
      verifyVolumeIndex(testmi);
    }
  }

  closeVolumeIndex(testmi);
}

/**********************************************************************/
static void threadParallel(void *arg)
{
  ThreadMI *threadmi = (ThreadMI *) arg;
  int firstCount = 8 * threadmi->testmi->geometry.records_per_chapter;

  // Verify the index
  threadVerifyVolumeIndex(threadmi->testmi, threadmi->zone,
                          threadmi->testmi->entryCounter);
  // Add 8 more chapters
  threadAddToVolumeIndex(threadmi->testmi, threadmi->zone,
                         threadmi->testmi->entryCounter, firstCount);
}

/**********************************************************************/
static void threadLookup(void *arg)
{
  TestMI *testmi = (TestMI *)arg;
  int j;
  for (j = 0; j < 8; j++) {
    long i;
    for (i = 0; i < testmi->entryCounter; i++) {
      uint64_t counter = i;
      struct uds_record_name name
        = hash_record_name(&counter, sizeof(counter));
      uint64_t virtual_chapter = uds_lookup_volume_index_name(testmi->mi, &name);
      if (virtual_chapter != NO_CHAPTER) {
        uint64_t chapter = counter / testmi->geometry.records_per_chapter;
        CU_ASSERT_EQUAL(virtual_chapter, chapter);
      }
    }
  }
}

/**********************************************************************/
static void testParallel(unsigned int numZones, bool sparse)
{
  TestMI *testmi = openVolumeIndex(numZones, sparse);

  // Add 2 chapters to the volume index
  addToVolumeIndex(testmi, 2 * testmi->geometry.records_per_chapter);
  verifyVolumeIndex(testmi);

  int i;
  for (i = 0; i < 2; i++) {
    // Launch a thread per zone to add 8 chapters and save the volume index
    ThreadMI threadmi[ZONES];
    unsigned int z;
    for (z = 0; z < numZones; z++) {
      char nameBuf[100];
      UDS_ASSERT_SUCCESS(uds_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                           "parallel%d", z));
      threadmi[z].testmi = testmi;
      threadmi[z].zone   = z;
      UDS_ASSERT_SUCCESS(vdo_create_thread(threadParallel, &threadmi[z],
                                           nameBuf, &threadmi[z].thread));
    }
    // Launch another thread to lookup names in parallel
    struct thread *lookupThread;
    UDS_ASSERT_SUCCESS(vdo_create_thread(threadLookup, testmi, "lookup",
                                         &lookupThread));
    // Join the threads
    vdo_join_threads(lookupThread);
    for (z = 0; z < numZones; z++) {
      vdo_join_threads(threadmi[z].thread);
    }
    saveVolumeIndex(threadmi->testmi);

    // Verify the 8 additional chapters
    testmi->entryCounter += 8 * testmi->geometry.records_per_chapter;
    verifyVolumeIndex(testmi);

    // Now restore and verify
    reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);
    verifyVolumeIndex(testmi);
  }
  closeVolumeIndex(testmi);
}

/**********************************************************************/
static void testEarlyLRU(unsigned int numZones, bool sparse)
{
  TestMI *testmi = openVolumeIndex(numZones, sparse);
  int recordsPerVolume = ((testmi->geometry.chapters_per_volume
                           - testmi->geometry.sparse_chapters_per_volume)
                          * testmi->geometry.records_per_chapter);
  addToVolumeIndex(testmi, recordsPerVolume);
  verifyVolumeIndex(testmi);
  // Trigger an early LRU in the volume index in only one zone
  overflowVolumeIndex(testmi);
  // Now save and restore the volume index
  saveVolumeIndex(testmi);
  reopenVolumeIndex(testmi, numZones, UDS_SUCCESS);
  closeVolumeIndex(testmi);
}

/**********************************************************************/
static void dense1ZoneTest(void)
{
  testMostlyEmpty(1, false);
  testChangingZones(1, false);
}

/**********************************************************************/
static void dense2ZoneTest(void)
{
  testMostlyEmpty(2, false);
  testChangingZones(2, false);
  testParallel(2, false);
  testEarlyLRU(2, false);
}

/**********************************************************************/
static void dense3ZoneTest(void)
{
  testMostlyEmpty(3, false);
  testChangingZones(3, false);
  testParallel(3, false);
  testEarlyLRU(3, false);
}

/**********************************************************************/
static void sparse1ZoneTest(void)
{
  testMostlyEmpty(1, true);
  testChangingZones(1, true);
}

/**********************************************************************/
static void sparse2ZoneTest(void)
{
  testMostlyEmpty(2, true);
  testChangingZones(2, true);
  testParallel(2, true);
  testEarlyLRU(2, true);
}

/**********************************************************************/
static void sparse3ZoneTest(void)
{
  testMostlyEmpty(3, true);
  testChangingZones(3, true);
  testParallel(3, true);
  testEarlyLRU(3, true);
}

/**********************************************************************/
static const CU_TestInfo volumeIndexTests[] = {
  {"Dense 1 zone",  dense1ZoneTest },
  {"Dense 2 zone",  dense2ZoneTest },
  {"Dense 3 zone",  dense3ZoneTest },
  {"Sparse 1 zone", sparse1ZoneTest },
  {"Sparse 2 zone", sparse2ZoneTest },
  {"Sparse 3 zone", sparse3ZoneTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "VolumeIndex_n2",
  .tests = volumeIndexTests,
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
