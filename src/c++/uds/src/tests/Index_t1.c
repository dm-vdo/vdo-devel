// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "index.h"
#include "volume-index.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "testRequests.h"

// Create some test hashes and metadata.
enum {
  NUM_HASHES = 8
};

static struct uds_record_name hashes[NUM_HASHES];
static struct uds_record_data metas[NUM_HASHES];

static struct configuration *config;
static struct configuration *smallConfig;

static struct uds_index *theIndex;

/**
 * The suite initialization function.
 **/
static void indexInitSuite(const char *name)
{
  unsigned int i, j;
  for (i = 0; i < NUM_HASHES; i++) {
    hashes[i].name[0] = i;
  }

  for (i = 0; i < NUM_HASHES; i++) {
    for (j = 0; j < UDS_RECORD_DATA_SIZE; j++) {
      metas[i].data[j] = i;
    }
  }

  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = name,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));

  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &smallConfig));
  size_t smallBytesPerPage = 128 * BYTES_PER_RECORD * smallConfig->zone_count;
  resizeDenseConfiguration(smallConfig, smallBytesPerPage, 1, 10);
  initialize_test_requests();
}

/**
 * The suite cleanup function.
 **/
static void indexCleanSuite(void)
{
  uninitialize_test_requests();
  uds_free_configuration(config);
  uds_free_configuration(smallConfig);
}

/**
 * The index creation utility.
 */
static void createIndex(bool load, struct configuration *config)
{
  enum uds_open_index_type openType = (load ? UDS_NO_REBUILD : UDS_CREATE);
  UDS_ASSERT_SUCCESS(make_index(config, openType, NULL, NULL, &theIndex));
  CU_ASSERT_PTR_NOT_NULL(theIndex);
}

/**********************************************************************/
static void indexAddAndCheck(unsigned int hashIndex,
                             unsigned int metaInIndex,
                             bool         expected,
                             unsigned int expectedMetaIndex)
{
  struct uds_request request = {
    .record_name  = hashes[hashIndex],
    .new_metadata = metas[metaInIndex],
    .type         = UDS_POST,
  };
  verify_test_request(theIndex, &request, expected, &metas[expectedMetaIndex]);
}

/**********************************************************************/
static void indexAdd(unsigned int hashIndex, unsigned int metaInIndex)
{
  indexAddAndCheck(hashIndex, metaInIndex, false, 0);
}

/**********************************************************************/
static void indexDelete(unsigned int hashIndex, bool expected)
{
  struct uds_request request = {
    .record_name = hashes[hashIndex],
    .type        = UDS_DELETE,
  };
  verify_test_request(theIndex, &request, expected, NULL);
}

/**********************************************************************/
static void indexLookup(unsigned int hashIndex,
                        bool         expected,
                        unsigned int expectedMetaIndex)
{
  struct uds_request request = {
    .record_name = hashes[hashIndex],
    .type        = UDS_QUERY,
  };
  verify_test_request(theIndex, &request, expected, &metas[expectedMetaIndex]);
}

/**********************************************************************/
static void indexUpdate(unsigned int hashIndex,
                        unsigned int metaInIndex,
                        bool         expected,
                        unsigned int expectedMetaIndex)
{
  struct uds_request request = {
    .record_name  = hashes[hashIndex],
    .new_metadata = metas[metaInIndex],
    .type         = UDS_UPDATE,
  };
  verify_test_request(theIndex, &request, expected, &metas[expectedMetaIndex]);
}

/**********************************************************************/
static void addAllHashes(bool expectFound)
{
  unsigned int i;
  for (i = 0; i < NUM_HASHES; i++) {
    indexAddAndCheck(i, (expectFound ? (i + 1) % NUM_HASHES : i),
                     expectFound, i);
  }
}

/**********************************************************************/
static struct volume_index_record assertFoundInMI(unsigned int hashIndex)
{
  struct volume_index_record mir;
  UDS_ASSERT_SUCCESS(get_volume_index_record(theIndex->volume_index,
                                             &hashes[hashIndex], &mir));
  CU_ASSERT_TRUE(mir.is_found);
  return mir;
}

/**********************************************************************/
static void assertInOpenChapter(unsigned int hashIndex)
{
  struct volume_index_record mir = assertFoundInMI(hashIndex);
  CU_ASSERT_EQUAL(mir.virtual_chapter,
                  theIndex->zones[mir.zone_number]->newest_virtual_chapter);
}

/**********************************************************************/
static void assertNotInOpenChapter(unsigned int hashIndex)
{
  struct volume_index_record mir = assertFoundInMI(hashIndex);
  CU_ASSERT_NOT_EQUAL(mir.virtual_chapter,
                      theIndex->zones[mir.zone_number]->newest_virtual_chapter);
}

/** Tests **/

/**********************************************************************/
static void addTest(void)
{
  createIndex(false, config);
  indexAdd(1, 1);
  indexAddAndCheck(1, 2, true, 1);
  indexAddAndCheck(1, 3, true, 1);
  free_index(theIndex);
}

/**********************************************************************/
static void updateTest(void)
{
  createIndex(false, config);
  indexAdd(1, 1);
  indexUpdate(1, 2, true, 1);
  indexAddAndCheck(1, 3, true, 2);
  free_index(theIndex);
}

/**********************************************************************/
static void updateInsertTest(void)
{
  createIndex(false, config);
  indexUpdate(1, 1, false, NUM_HASHES - 1);
  free_index(theIndex);
}

/**********************************************************************/
static void removeTest(void)
{
  createIndex(false, config);
  indexDelete(1, false);
  indexAdd(1, 1);
  indexAddAndCheck(1, 2, true, 1);
  indexDelete(1, true);
  indexAdd(1, 1);
  free_index(theIndex);
}

/**********************************************************************/
static void lruAddTest(void)
{
  createIndex(false, smallConfig);
  indexAdd(1, 1);
  indexAddAndCheck(1, 2, true, 1);
  free_index(theIndex);
}

/**********************************************************************/
static void lruAdd2Test(void)
{
  createIndex(false, smallConfig);
  indexAdd(1, 1);
  indexAdd(2, 2);
  indexAddAndCheck(1, 3, true, 1);
  indexAddAndCheck(1, 4, true, 1);
  indexAddAndCheck(2, 5, true, 2);
  free_index(theIndex);
}

/**********************************************************************/
static void lruUpdateTest(void)
{
  createIndex(false, smallConfig);
  indexAdd(1, 1);
  indexUpdate(1, 2, true, 1);
  indexAddAndCheck(1, 3, true, 2);
  free_index(theIndex);
}

/**********************************************************************/
static void lruUpdate2Test(void)
{
  createIndex(false, smallConfig);
  indexAdd(1, 1);
  indexAdd(2, 2);
  indexUpdate(1, 3, true, 1);
  indexUpdate(1, 4, true, 3);
  free_index(theIndex);
}

/**********************************************************************/
static void lruLookupTest(void)
{
  createIndex(false, smallConfig);
  indexAdd(1, 1);
  assertInOpenChapter(1);
  indexLookup(1, true, 1);
  assertInOpenChapter(1);
  fillChapterRandomly(theIndex);
  assertNotInOpenChapter(1);
  indexLookup(1, true, 1);
  assertInOpenChapter(1);
  free_index(theIndex);
}

/**********************************************************************/
static void saveLoadTest(void)
{
  createIndex(false, config);
  addAllHashes(false);

  uint64_t     newestChapter = theIndex->newest_virtual_chapter;
  uint64_t     oldestChapter = theIndex->oldest_virtual_chapter;

  unsigned int newestPhysicalChapter
    = map_to_physical_chapter(theIndex->volume->geometry, newestChapter);
  unsigned int oldestPhysicalChapter
    = map_to_physical_chapter(theIndex->volume->geometry, oldestChapter);

  UDS_ASSERT_SUCCESS(save_index(theIndex));
  free_index(theIndex);

  createIndex(true, config);
  addAllHashes(true);

  CU_ASSERT_EQUAL(newestChapter, theIndex->newest_virtual_chapter);
  CU_ASSERT_EQUAL(oldestChapter, theIndex->oldest_virtual_chapter);
  CU_ASSERT_EQUAL(newestPhysicalChapter,
                  map_to_physical_chapter(theIndex->volume->geometry,
                                          theIndex->newest_virtual_chapter));
  CU_ASSERT_EQUAL(oldestPhysicalChapter,
                  map_to_physical_chapter(theIndex->volume->geometry,
                                          theIndex->oldest_virtual_chapter));
  free_index(theIndex);

  createIndex(false, config);
  addAllHashes(false);
  free_index(theIndex);
}

/**********************************************************************/
static const CU_TestInfo indexTests[] = {
  {"Add",           addTest },
  {"Update",        updateTest },
  {"Update Insert", updateInsertTest },
  {"Remove",        removeTest },
  {"LRU Add",       lruAddTest },
  {"LRU Add2",      lruAdd2Test },
  {"LRU Update",    lruUpdateTest },
  {"LRU Update2",   lruUpdate2Test },
  {"LRU Lookup",    lruLookupTest },
  {"Save Load",     saveLoadTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Index_t1",
  .initializerWithIndexName = indexInitSuite,
  .cleaner                  = indexCleanSuite,
  .tests                    = indexTests,
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
