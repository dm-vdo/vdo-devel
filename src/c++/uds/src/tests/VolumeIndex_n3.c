// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "volume-index.h"
#include "testPrototypes.h"

/**********************************************************************/
static void fillChapter(struct volume_index *miptr, uint64_t chapter,
                        int numRecords)
{
  static uint64_t counter = 0;
  set_volume_index_open_chapter(miptr, chapter);
  int i;
  for (i = 0; i < numRecords; i++) {
    struct uds_record_name name = hash_record_name(&counter, sizeof(counter));
    counter++;
    struct volume_index_record record;
    UDS_ASSERT_SUCCESS(get_volume_index_record(miptr, &name, &record));
    UDS_ASSERT_SUCCESS(put_volume_index_record(&record, chapter));
  }
}

/**********************************************************************/
static void testEarlyLRU(int numZones)
{
  enum { meanDelta   = 1 << 16 };
  enum { numRecords  = 1024 };
  enum { numChapters = 1024 };

  // Make the test geometry
  struct geometry geometry;
  memset(&geometry, 0, sizeof(struct geometry));
  geometry.chapters_per_volume = numChapters;
  geometry.records_per_chapter = numRecords;

  // Make the test configuration
  struct configuration config;
  memset(&config, 0, sizeof(struct configuration));
  config.geometry = &geometry;
  config.volume_index_mean_delta = meanDelta;
  config.zone_count = numZones;

  // Create the volume index
  struct volume_index *miptr;
  struct volume_index_stats miStats;
  UDS_ASSERT_SUCCESS(make_volume_index(&config, 0, &miptr));

  // Fill the index, then fill it again
  unsigned int chapter = 0;
  while (chapter < 2 * numChapters) {
    fillChapter(miptr, chapter, numRecords);
    ++chapter;
    get_volume_index_combined_stats(miptr, &miStats);
    CU_ASSERT_EQUAL(miStats.overflow_count, 0);
    CU_ASSERT_EQUAL(miStats.early_flushes, 0);
  }

  // Fill the index again with 12.5% more records than usual
  while (chapter < 3 * numChapters) {
#ifdef __KERNEL__
    // On slower machines, we can cause "soft lockup" complaints if we
    // don't yield.
    cond_resched();
#endif
    fillChapter(miptr, chapter, numRecords + numRecords / 8);
    ++chapter;
    get_volume_index_combined_stats(miptr, &miStats);
    CU_ASSERT_EQUAL(miStats.overflow_count, 0);
  }
  CU_ASSERT_NOT_EQUAL(miStats.early_flushes, 0);

  free_volume_index(miptr);
}

/**********************************************************************/
static void zone1Test(void)
{
  testEarlyLRU(1);
}

/**********************************************************************/
static void zone2Test(void)
{
  testEarlyLRU(2);
}

/**********************************************************************/
static void zone3Test(void)
{
  testEarlyLRU(3);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"Early LRU 1 zone", zone1Test },
  {"Early LRU 2 zone", zone2Test },
  {"Early LRU 3 zone", zone3Test },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "VolumeIndex_n3",
  .tests = tests,
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
