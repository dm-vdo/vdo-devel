// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "errors.h"
#include "hash-utils.h"
#include "open-chapter.h"
#include "string-utils.h"
#include "testPrototypes.h"

static const unsigned            CHAPTER_COUNT = 16;
static struct configuration     *conf;
static struct geometry          *geometry;
static struct open_chapter_zone *openChapter;

/**********************************************************************/
static void initializeTest(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
  };

  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &conf));
  resizeDenseConfiguration(conf, conf->geometry->bytes_per_page / 8,
                           conf->geometry->record_pages_per_chapter / 2,
                           CHAPTER_COUNT);
  geometry = conf->geometry;

  UDS_ASSERT_SUCCESS(make_open_chapter(geometry, 1, &openChapter));
}

/**********************************************************************/
static void finishTest(void)
{
  free_open_chapter(openChapter);
  uds_free_configuration(conf);
}

/**********************************************************************/
static void openChapterSearch(struct uds_record_name *name,
                              struct uds_record_data *data,
                              bool                    expectFound)
{
  bool found;
  search_open_chapter(openChapter, name, data, &found);
  CU_ASSERT_EQUAL(found, expectFound);
}

/**********************************************************************/
static void testEmpty(void)
{
  struct uds_record_name name;
  struct uds_record_name zero;
  struct uds_record_data meta;

  createRandomBlockName(&name);
  memset(&zero, 0, sizeof(zero));

  CU_ASSERT_EQUAL(0, openChapter->size);
  CU_ASSERT_EQUAL(0, openChapter->deletions);
  openChapterSearch(&zero, &meta, false);
  openChapterSearch(&name, &meta, false);

  // Opening an empty chapter should work, but do nothing.
  reset_open_chapter(openChapter);
  CU_ASSERT_EQUAL(0, openChapter->size);
  CU_ASSERT_EQUAL(0, openChapter->deletions);
  openChapterSearch(&zero, &meta, false);
}

/**********************************************************************/
static void put(struct uds_record_name *name, struct uds_record_data *data,
                bool expectFull)
{
  unsigned int remaining = put_open_chapter(openChapter, name, data);

  CU_ASSERT_EQUAL((remaining == 0), expectFull);
}

/**********************************************************************/
static void putNotFull(struct uds_record_name *name,
                       struct uds_record_data *data)
{
  put(name, data, false);
}

/**********************************************************************/
static void testSingleton(void)
{
  struct uds_record_name name1;
  struct uds_record_data meta1;
  struct uds_record_name name2;
  struct uds_record_data meta2;
  struct uds_record_data metaOut;

  createRandomBlockName(&name1);
  createRandomMetadata(&meta1);
  createRandomBlockName(&name2);
  createRandomMetadata(&meta2);

  // Add one record to the chapter.
  putNotFull(&name1, &meta1);
  CU_ASSERT_EQUAL(1, openChapter->size);
  CU_ASSERT_EQUAL(0, openChapter->deletions);

  // Make sure we see the record we added.
  openChapterSearch(&name1, &metaOut, true);
  UDS_ASSERT_BLOCKDATA_EQUAL(&meta1, &metaOut);

  // We shouldn't see a record we didn't add.
  openChapterSearch(&name2, &metaOut, false);

  // Test modification of the record that's already there.
  putNotFull(&name1, &meta2);
  openChapterSearch(&name1, &metaOut, true);
  UDS_ASSERT_BLOCKDATA_EQUAL(&meta2, &metaOut);

  // Delete the record and check that it's not there.
  remove_from_open_chapter(openChapter, &name1);
  CU_ASSERT_EQUAL(1, openChapter->size);
  CU_ASSERT_EQUAL(1, openChapter->deletions);
  openChapterSearch(&name1, &metaOut, false);
}

/**********************************************************************/
static void testFilling(void)
{
  struct uds_record_name name;
  struct uds_record_data meta;

  // Almost fill the chapter with randomly-generated data.
  unsigned int fullLessOne = openChapter->capacity - 1;
  unsigned int i;
  for (i = 0; i < fullLessOne; i++) {
    CU_ASSERT_EQUAL(i, openChapter->size - openChapter->deletions);
    createRandomBlockName(&name);
    createRandomMetadata(&meta);
    putNotFull(&name, &meta);
  }

  CU_ASSERT_EQUAL(fullLessOne, openChapter->size - openChapter->deletions);

  // Add one more entry. It should indicate the chapter is full.
  createRandomBlockName(&name);
  createRandomMetadata(&meta);

  put(&name, &meta, true);
  CU_ASSERT_EQUAL(openChapter->capacity,
                  openChapter->size - openChapter->deletions);

  // Add one more entry. It should be ignored.
  createRandomBlockName(&name);
  createRandomMetadata(&meta);

  CU_ASSERT_EQUAL(0, put_open_chapter(openChapter, &name, &meta));
  CU_ASSERT_EQUAL(openChapter->capacity,
                  openChapter->size - openChapter->deletions);
}

/**********************************************************************/
static void testQuadraticProbing(void)
{
  /*
   * Test that we can always insert records into the open chapter (via
   * quadratic probing) up to its capacity. Repeatedly add names that have
   * hash slot 0. The failure mode is that put_open_chapter() loops indefinitely.
   */
  struct open_chapter_zone *theChapter;
  geometry->records_per_chapter = 16;
  unsigned int zoneCount = 3;
  unsigned int recordsPerZone = 5;
  UDS_ASSERT_SUCCESS(make_open_chapter(geometry, zoneCount, &theChapter));
  CU_ASSERT_EQUAL(recordsPerZone, theChapter->capacity);

  unsigned int i;
  for (i = 0; i < recordsPerZone; ++i) {
    struct uds_record_name name;
    struct uds_record_data data;
    do {
      createRandomBlockName(&name);
      memcpy(&data.data, &name.name, UDS_RECORD_NAME_SIZE);
    } while (name_to_hash_slot(&name, theChapter->slot_count) != 0);
    CU_ASSERT_EQUAL(recordsPerZone - i - 1,
                    put_open_chapter(theChapter, &name, &data));
  }
  free_open_chapter(theChapter);
}

/**********************************************************************/
static const CU_TestInfo openChapterTests[] = {
  {"Empty",                         testEmpty               },
  {"Singleton",                     testSingleton           },
  {"Filling",                       testFilling             },
  {"Quadratic Probing",             testQuadraticProbing    },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "OpenChapter_t1",
  .initializer = initializeTest,
  .cleaner     = finishTest,
  .tests       = openChapterTests,
};

/**
 * Entry point required by the module loader. Return a pointer to the
 * const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
