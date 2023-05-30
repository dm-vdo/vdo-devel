// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "index.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "open-chapter.h"
#include "testPrototypes.h"
#include "testRequests.h"

static struct configuration *config;
static struct io_factory    *factory;
static struct uds_index     *theIndex;
static uint64_t              scratchOffset;
static uint64_t              chapterBlocks;

/**********************************************************************/
static void initializeTest(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .name = getTestIndexName(),
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, config->geometry->bytes_per_page / 8,
                           config->geometry->record_pages_per_chapter / 2, 16);
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_CREATE, NULL, NULL, &theIndex));
  UDS_ASSERT_SUCCESS(uds_make_io_factory(getTestIndexName(), &factory));

  UDS_ASSERT_SUCCESS(uds_compute_index_size(&params, &scratchOffset));
  scratchOffset = DIV_ROUND_UP(scratchOffset, UDS_BLOCK_SIZE);
  chapterBlocks
    = DIV_ROUND_UP(uds_compute_saved_open_chapter_size(config->geometry),
                   UDS_BLOCK_SIZE);

  initialize_test_requests();
}

/**********************************************************************/
static void finishTest(void)
{
  uninitialize_test_requests();
  uds_put_io_factory(factory);
  uds_free_configuration(config);
  uds_free_index(theIndex);
}

/**********************************************************************/
__attribute__((warn_unused_result))
static struct buffered_reader *openBufferedReaderForChapter(void)
{
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, scratchOffset, chapterBlocks, &reader));
  return reader;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static struct buffered_writer *openBufferedWriterForChapter(void)
{
  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(uds_make_buffered_writer(factory, scratchOffset, chapterBlocks, &writer));
  return writer;
}

/**********************************************************************/
static void requestIndex(struct uds_record_name *hash,
                         struct uds_record_data *newMetadata)
{
  struct uds_request request = {
    .record_name  = *hash,
    .new_metadata = *newMetadata,
    .type         = UDS_POST,
  };
  verify_test_request(theIndex, &request, false, NULL);
}

/**********************************************************************/
static void testSaveLoadEmpty(void)
{
  struct buffered_writer *writer = openBufferedWriterForChapter();
  UDS_ASSERT_SUCCESS(uds_save_open_chapter(theIndex, writer));
  uds_free_buffered_writer(writer);
  uds_reset_open_chapter(theIndex->zones[0]->open_chapter);

  struct buffered_reader *reader = openBufferedReaderForChapter();
  UDS_ASSERT_SUCCESS(uds_load_open_chapter(theIndex, reader));
  uds_free_buffered_reader(reader);

  unsigned int i;
  for (i = 0; i < theIndex->zone_count; i++) {
    CU_ASSERT_EQUAL(0, theIndex->zones[i]->open_chapter->size);
  }
}

/**********************************************************************/
static void testSaveLoadWithData(void)
{
  // Create some random records to put in the open chapter.
  int totalRecords = theIndex->volume->geometry->records_per_chapter / 2;
  struct uds_volume_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(totalRecords,
                                  struct uds_volume_record, "test records",
                                  &records));

  int i;
  for (i = 0; i < totalRecords; i++) {
    createRandomBlockName(&records[i].name);
    createRandomMetadata(&records[i].data);
    requestIndex(&records[i].name, &records[i].data);
  }

  // Save the open chapter file and assert that all records can be found.
  struct buffered_writer *writer = openBufferedWriterForChapter();
  UDS_ASSERT_SUCCESS(uds_save_open_chapter(theIndex, writer));
  uds_free_buffered_writer(writer);
  uds_reset_open_chapter(theIndex->zones[0]->open_chapter);

  struct buffered_reader *reader = openBufferedReaderForChapter();
  UDS_ASSERT_SUCCESS(uds_load_open_chapter(theIndex, reader));
  uds_free_buffered_reader(reader);

  for (i = 0; i < totalRecords; i++) {
    unsigned int zone = uds_get_volume_index_zone(theIndex->volume_index, &records[i].name);
    struct uds_record_data metadata;
    bool found = false;

    uds_search_open_chapter(theIndex->zones[zone]->open_chapter,
                            &records[i].name,
                            &metadata,
                            &found);
    CU_ASSERT_TRUE(found);
    UDS_ASSERT_BLOCKDATA_EQUAL(&records[i].data, &metadata);
  }

  UDS_FREE(records);
}

/**********************************************************************/
static void testSaveLoadWithDiscard(void)
{
  uds_free_index(theIndex);
  config->zone_count = 1;
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_CREATE, NULL, NULL, &theIndex));

  // Fill a one-zone open chapter as full as possible.
  int totalRecords = theIndex->volume->geometry->records_per_chapter - 1;
  struct uds_volume_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(totalRecords,
                                  struct uds_volume_record, "test records",
                                  &records));

  int i;
  for (i = 0; i < totalRecords; i++) {
    createRandomBlockName(&records[i].name);
    createRandomMetadata(&records[i].data);
    requestIndex(&records[i].name, &records[i].data);
  }

  // Save the open chapter file, and reload with a three-zone index.
  struct buffered_writer *writer = openBufferedWriterForChapter();
  UDS_ASSERT_SUCCESS(uds_save_open_chapter(theIndex, writer));
  uds_free_buffered_writer(writer);
  uds_free_index(theIndex);

  enum { ZONE_COUNT = 3 };
  config->zone_count = ZONE_COUNT;
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_LOAD, NULL, NULL, &theIndex));
  int z;
  for (z = 0; z < ZONE_COUNT; z++) {
    uds_reset_open_chapter(theIndex->zones[z]->open_chapter);
  }

  struct buffered_reader *reader = openBufferedReaderForChapter();
  UDS_ASSERT_SUCCESS(uds_load_open_chapter(theIndex, reader));
  uds_free_buffered_reader(reader);

  // At least one zone will have more records than will fit in the
  // openChapterZone, so make sure the extras are discarded.
  unsigned int recordsPerZone[ZONE_COUNT];
  memset(recordsPerZone, 0, ZONE_COUNT * sizeof(unsigned int));
  for (i = 0; i < totalRecords; i++) {
    unsigned int zone = uds_get_volume_index_zone(theIndex->volume_index, &records[i].name);
    recordsPerZone[zone]++;
    struct uds_record_data metadata;
    bool found = false;
    struct open_chapter_zone *openChapter
      = theIndex->zones[zone]->open_chapter;

    uds_search_open_chapter(openChapter, &records[i].name, &metadata, &found);
    CU_ASSERT_TRUE(found == (recordsPerZone[zone] < openChapter->capacity));
    if (found) {
      UDS_ASSERT_BLOCKDATA_EQUAL(&records[i].data, &metadata);
    }
  }

  int newTotalRecords = 0;
  for (z = 0; z < ZONE_COUNT; z++) {
    newTotalRecords += theIndex->zones[z]->open_chapter->size;
  }

  CU_ASSERT_TRUE(totalRecords > newTotalRecords);
  UDS_FREE(records);
}

/**********************************************************************/
static void modifyOpenChapter(off_t offset, const char *data)
{
  struct buffered_writer *writer = openBufferedWriterForChapter();
  UDS_ASSERT_SUCCESS(uds_save_open_chapter(theIndex, writer));
  uds_free_buffered_writer(writer);

  u8 *block;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(UDS_BLOCK_SIZE, u8, __func__, &block));
  struct buffered_reader *reader = openBufferedReaderForChapter();
  UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, block, UDS_BLOCK_SIZE));
  uds_free_buffered_reader(reader);

  CU_ASSERT_TRUE(offset >= 0);
  CU_ASSERT_TRUE(offset + strlen(data) <= UDS_BLOCK_SIZE);
  memcpy(block + offset, data, strlen(data));

  writer = openBufferedWriterForChapter();
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, block, UDS_BLOCK_SIZE));
  UDS_ASSERT_SUCCESS(uds_flush_buffered_writer(writer));
  uds_free_buffered_writer(writer);
  UDS_FREE(block);
}

/**********************************************************************/
static void loadModifiedOpenChapter(void)
{
  struct buffered_reader *reader = openBufferedReaderForChapter();
  struct uds_index *restoringIndex = NULL;
  UDS_ASSERT_ERROR(UDS_CORRUPT_DATA,
                   uds_load_open_chapter(restoringIndex, reader));
  uds_free_buffered_reader(reader);
  uds_free_index(restoringIndex);
}

/**********************************************************************/
static void testBadMagic(void)
{
  modifyOpenChapter(0, "FOOBA");
  loadModifiedOpenChapter();
}

static const unsigned int VERSION_OFFSET = 5;

/**********************************************************************/
static void testBadVersion(void)
{
  modifyOpenChapter(VERSION_OFFSET, "XXXXX");
  loadModifiedOpenChapter();
}

/**********************************************************************/
static const CU_TestInfo openChapterSaveLoadTests[] = {
  {"Empty Chapter",       testSaveLoadEmpty        },
  {"Partial Chapter",     testSaveLoadWithData     },
  {"Load with Discards",  testSaveLoadWithDiscard  },
  {"BadMagic",            testBadMagic             },
  {"BadVersion",          testBadVersion           },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "OpenChapter_t2",
  .initializer = initializeTest,
  .cleaner     = finishTest,
  .tests       = openChapterSaveLoadTests,
};

/**
 * Entry point required by the module loader. Return a pointer to the
 * const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
