// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"
#include "volume.h"

static void recordPageTest(int numRecords)
{
  size_t bytesPerPage = BYTES_PER_RECORD * numRecords;
  struct uds_parameters params = {
    .memory_size = 1,
  };
  struct configuration *conf;
  UDS_ASSERT_SUCCESS(make_configuration(&params, &conf));
  resizeDenseConfiguration(conf, bytesPerPage, 1, 1);
  struct geometry *g = conf->geometry;

  byte *recordPage;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(bytesPerPage, byte, __func__, &recordPage));
  const struct uds_chunk_record **recordPointers;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(g->records_per_page,
                                  const struct uds_chunk_record *,
                                  __func__, &recordPointers));
  struct uds_chunk_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE((bytesPerPage /
                                   sizeof(struct uds_chunk_record)),
                                  struct uds_chunk_record, __func__,
                                  &records));

  // A fake volume but good enough for the encode_record_page interface
  struct volume *volume;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct volume, __func__, &volume));
  UDS_ASSERT_SUCCESS(make_radix_sorter(g->records_per_page,
                                       &volume->radix_sorter));
  volume->geometry        = g;
  volume->record_pointers = recordPointers;

  albPrint("===== Testing %zdK Byte Record Pages ====", bytesPerPage / 1024);
  ktime_t encodeTime = 0;
  ktime_t searchTime = 0;
  enum { REPETITIONS = 6000 };

  int repetition;
  for (repetition = 0; repetition < REPETITIONS; repetition++) {
    fill_randomly((byte *) records, bytesPerPage);
    ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);
    UDS_ASSERT_SUCCESS(encode_record_page(volume, records, recordPage));
    encodeTime += ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);

    startTime = current_time_ns(CLOCK_MONOTONIC);
    size_t i;
    for (i = 0; i < g->records_per_page; ++i) {
      struct uds_chunk_name name = records[i].name;
      struct uds_chunk_data metadata;

      bool found = search_record_page(recordPage, &name, g, &metadata);
      CU_ASSERT_TRUE(found);
      UDS_ASSERT_BLOCKDATA_EQUAL(&metadata, &records[i].data);
    }
    searchTime += ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);
  }

  char *encodeTotal, *encodeEach, *searchTotal, *searchEach;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&encodeTotal, encodeTime, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&encodeEach, encodeTime, REPETITIONS));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&searchTotal, searchTime, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&searchEach, searchTime,
                                        g->records_per_page * REPETITIONS));
  albPrint("Encoded %d pages in %s", REPETITIONS, encodeTotal);
  albPrint("Each page encoded in %s", encodeEach);
  albPrint("Searched %d entries in %s", g->records_per_page * REPETITIONS,
           searchTotal);
  albPrint("Each entry searched in %s", searchEach);
  UDS_FREE(encodeTotal);
  UDS_FREE(encodeEach);
  UDS_FREE(searchTotal);
  UDS_FREE(searchEach);

  free_radix_sorter(volume->radix_sorter);
  UDS_FREE(records);
  UDS_FREE(recordPage);
  UDS_FREE(recordPointers);
  UDS_FREE(volume);
  free_configuration(conf);
}

static void test64K(void)
{
  recordPageTest(1024);
}

static void test16K(void)
{
  recordPageTest(256);
}

static const CU_TestInfo tests[] = {
  {"64K Record Page", test64K},
  {"16K Record Page", test16K},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "RecordPage_t2",
  .tests = tests,
};

// =============================================================================
// Entry point required by the module loader. Return a pointer to the
// const CU_SuiteInfo structure.
// =============================================================================

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
