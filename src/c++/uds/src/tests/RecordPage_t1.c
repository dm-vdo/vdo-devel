// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "volume.h"

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
  struct uds_volume_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE((bytesPerPage /
                                   sizeof(struct uds_volume_record)),
                                  struct uds_volume_record, __func__,
                                  &records));
  get_random_bytes((byte *) records, bytesPerPage);

  struct volume *volume;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct volume, __func__, &volume));
  // A fake volume but good enough for the encode_record_page interface
  volume->geometry = g;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(g->records_per_page,
                                  const struct uds_volume_record *,
                                  __func__, &volume->record_pointers));
  UDS_ASSERT_SUCCESS(make_radix_sorter(g->records_per_page,
                                       &volume->radix_sorter));

  UDS_ASSERT_SUCCESS(encode_record_page(volume, records, recordPage));
  size_t i;
  for (i = 0; i < g->records_per_page; ++i) {
    const struct uds_record_name *name = &records[i].name;
    struct uds_record_data metadata;

    bool found = search_record_page(recordPage, name, g, &metadata);
    CU_ASSERT_TRUE(found);
    UDS_ASSERT_BLOCKDATA_EQUAL(&metadata, &records[i].data);
  }

  struct uds_record_name zero;
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
