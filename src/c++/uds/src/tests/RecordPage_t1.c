// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "random.h"
#include "record-page.h"
#include "testPrototypes.h"

/**********************************************************************/
static void testMemcmp(void)
{
  enum { SIZE =16 };
  byte basic[SIZE];
  fill_randomly(basic, SIZE);
  byte s1[SIZE], s2[SIZE];
  memcpy(s1, basic, SIZE);
  memcpy(s2, basic, SIZE);

  CU_ASSERT_EQUAL(0, memcmp(s1, s2, SIZE));
  int i1, i2, index;
  for (index = 0; index < SIZE; index++) {
    memcpy(s1, basic, SIZE);
    for (i1 = 0; i1 <= UCHAR_MAX; i1++) {
      byte b1 = i1;
      s1[index] = b1;
      memcpy(s2, basic, SIZE);
      if (index + 1 < SIZE) {
        s2[index + 1] = b1;
      }
      for (i2 = 0; i2 <= UCHAR_MAX; i2++) {
        byte b2 = i2;
        s2[index] = b2;
        if (index + 1 < SIZE) {
          s1[index + 1] = b2;
        }
        CU_ASSERT_EQUAL(b1 < b2,  memcmp(s1, s2, SIZE) < 0);
        CU_ASSERT_EQUAL(b1 == b2, memcmp(s1, s2, SIZE) == 0);
        CU_ASSERT_EQUAL(b1 > b2,  memcmp(s1, s2, SIZE) > 0);
      }
    }
  }
}

/**********************************************************************/
static void testSearchRecordPage(void)
{
  unsigned int numRecords = 1024;
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
  struct uds_chunk_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE((bytesPerPage /
                                   sizeof(struct uds_chunk_record)),
                                  struct uds_chunk_record, __func__,
                                  &records));
  fill_randomly((byte *) records, bytesPerPage);

  struct volume *volume;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct volume, __func__, &volume));
  // A fake volume but good enough for the encode_record_page interface
  volume->geometry = g;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(g->records_per_page,
                                  const struct uds_chunk_record *,
                                  __func__, &volume->record_pointers));
  UDS_ASSERT_SUCCESS(make_radix_sorter(g->records_per_page,
                                       &volume->radix_sorter));

  UDS_ASSERT_SUCCESS(encode_record_page(volume, records, recordPage));
  size_t i;
  for (i = 0; i < g->records_per_page; ++i) {
    const struct uds_chunk_name *name = &records[i].name;
    struct uds_chunk_data metadata;

    bool found = search_record_page(recordPage, name, g, &metadata);
    CU_ASSERT_TRUE(found);
    UDS_ASSERT_BLOCKDATA_EQUAL(&metadata, &records[i].data);
  }

  struct uds_chunk_name zero;
  memset(&zero, 0, sizeof(zero));
  bool found = search_record_page(recordPage, &zero, g, NULL);
  CU_ASSERT_FALSE(found);

  UDS_FREE(records);
  UDS_FREE(recordPage);
  UDS_FREE(volume->record_pointers);
  free_radix_sorter(volume->radix_sorter);
  UDS_FREE(volume);
  free_configuration(conf);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"memcmp",             testMemcmp},
  {"Search record page", testSearchRecordPage},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "RecordPage_t1",
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
