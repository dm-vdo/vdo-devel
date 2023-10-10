// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "index.h"
#include "logger.h"
#include "testPrototypes.h"
#include "testRequests.h"

static struct block_device *testDevice;

// The metadata we will use in this suite
static struct uds_record_data cd1, cd2;
static struct uds_index *testIndex;

/**********************************************************************/
static struct uds_index *recreateTestIndex(enum uds_open_index_type openType)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  struct uds_index *index;
  UDS_ASSERT_SUCCESS(uds_make_index(config, openType, NULL, NULL, &index));
  uds_free_configuration(config);
  return index;
}

/**********************************************************************/
static void initSuite(struct block_device *bdev)
{
  testDevice = bdev;
  createRandomMetadata(&cd1);
  createRandomMetadata(&cd2);

  testIndex = recreateTestIndex(UDS_CREATE);
  initialize_test_requests();
}

/**********************************************************************/
static void cleanSuite(void)
{
  uninitialize_test_requests();
  uds_free_index(UDS_FORGET(testIndex));
}

/**********************************************************************/
static noinline void deleteChunk(struct uds_index             *index,
                                 const struct uds_record_name *name,
                                 bool                          exists)
{
  struct uds_request request = {
    .record_name = *name,
    .type        = UDS_DELETE,
  };
  verify_test_request(index, &request, exists, NULL);
}

/**********************************************************************/
static noinline
void expectChunk(struct uds_index             *index,
                 const struct uds_record_name *name,
                 const struct uds_record_data *cdExpected,
                 enum uds_index_region         expectedLocation)
{
  struct uds_request request = {
    .record_name = *name,
    .type        = UDS_QUERY_NO_UPDATE,
  };
  verify_test_request(index, &request, true, cdExpected);
  CU_ASSERT_EQUAL(request.location, expectedLocation);
}

/**********************************************************************/
static noinline void expectMissingChunk(struct uds_index             *index,
                                        const struct uds_record_name *name)
{
  struct uds_request request = {
    .record_name = *name,
    .type        = UDS_QUERY_NO_UPDATE,
  };
  verify_test_request(index, &request, false, NULL);
}

/**********************************************************************/
static void expectSurvivingChunk(struct uds_index             *index,
                                 const struct uds_record_name *name,
                                 const struct uds_record_data *cdExpected)
{
  /*
   * This is a chunk that has been deleted. Because of a rebuild or a
   * collision in the volume index, you can still find the chunk. This
   * means that the lookup will succeed, although it is not required to.
   * For testing purposes, we are interested when this expectation fails.
   */
  expectChunk(index, name, cdExpected, UDS_LOCATION_IN_DENSE);
}

/**********************************************************************/
static noinline void insertChunk(struct uds_index             *index,
                                 const struct uds_record_name *name,
                                 const struct uds_record_data *cd)
{
  struct uds_request request = {
    .record_name  = *name,
    .new_metadata = *cd,
    .type         = UDS_UPDATE,
  };
  verify_test_request(index, &request, false, NULL);
}

/**********************************************************************/
static void insertRandomChunk(struct uds_index             *index,
                              struct uds_record_name       *name,
                              const struct uds_record_data *cd)
{
  createRandomBlockName(name);
  insertChunk(index, name, cd);
}

/**********************************************************************/
static void insertCollidingChunk(struct uds_index             *index,
                                 const struct uds_record_name *name1,
                                 struct uds_record_name       *name2,
                                 const struct uds_record_data *cd)
{
  createCollidingBlock(name1, name2);
  insertChunk(index, name2, cd);
}

/**********************************************************************/
static noinline void updateChunk(struct uds_index             *index,
                                 const struct uds_record_name *name,
                                 const struct uds_record_data *cdOld,
                                 const struct uds_record_data *cdNew)
{
  struct uds_request request = {
    .record_name  = *name,
    .new_metadata = *cdNew,
    .type         = UDS_UPDATE,
  };
  verify_test_request(index, &request, true, cdOld);
}

/**********************************************************************/
static struct uds_index *rebuildIndex(struct uds_index *index)
{
  fillChapterRandomly(index);
  uds_wait_for_idle_index(index);

  // Do a full rebuild from the volume file
  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  uds_free_index(index);
  return recreateTestIndex(UDS_LOAD);
}

/**********************************************************************/
static void simpleOpenTest(void)
{
  // Insert two chunks
  struct uds_record_name name1, name2;
  insertRandomChunk(testIndex, &name1, &cd1);
  insertRandomChunk(testIndex, &name2, &cd2);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_OPEN_CHAPTER);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_OPEN_CHAPTER);

  // Delete chunk1
  deleteChunk(testIndex, &name1, true);
  expectMissingChunk(testIndex, &name1);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_OPEN_CHAPTER);

  // Rebuild the index from the volume file
  testIndex = rebuildIndex(testIndex);
  expectMissingChunk(testIndex, &name1);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
}

/**********************************************************************/
static void threeDeletesTest(void)
{
  // Insert 3 chunks into the open chapter and delete them
  struct uds_record_name name1, name2, name3;
  insertRandomChunk(testIndex, &name1, &cd1);
  insertRandomChunk(testIndex, &name2, &cd1);
  insertRandomChunk(testIndex, &name3, &cd1);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_OPEN_CHAPTER);
  expectChunk(testIndex, &name2, &cd1, UDS_LOCATION_IN_OPEN_CHAPTER);
  expectChunk(testIndex, &name3, &cd1, UDS_LOCATION_IN_OPEN_CHAPTER);
  deleteChunk(testIndex, &name1, true);
  deleteChunk(testIndex, &name2, true);
  deleteChunk(testIndex, &name3, true);

  // Testing closing a chapter with 3 deleted chunks.
  fillChapterRandomly(testIndex);

  // Expect the chunks to be missing
  expectMissingChunk(testIndex, &name1);
  expectMissingChunk(testIndex, &name2);
  expectMissingChunk(testIndex, &name3);

  // Rebuild the index from the volume file
  testIndex = rebuildIndex(testIndex);
  expectMissingChunk(testIndex, &name1);
  expectMissingChunk(testIndex, &name2);
  expectMissingChunk(testIndex, &name3);
}

/**********************************************************************/
static void simpleClosedTest(void)
{
  // Insert a chunk into the open chapter, and then fill the chapter
  struct uds_record_name name1;
  insertRandomChunk(testIndex, &name1, &cd1);
  fillChapterRandomly(testIndex);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);

  // Now the chunk is in a closed chapter, so delete it
  deleteChunk(testIndex, &name1, true);
  expectMissingChunk(testIndex, &name1);

  // Rebuild the index from the volume file.  The deleted chunk comes back.
  testIndex = rebuildIndex(testIndex);
  expectSurvivingChunk(testIndex, &name1, &cd1);
}

/**********************************************************************/
static void collisionClosedTest(void)
{
  // Insert two colliding chunks into the open chapter, and then fill the
  // chapter
  struct uds_record_name name1, name2;
  insertRandomChunk(testIndex, &name1, &cd1);
  insertCollidingChunk(testIndex, &name1, &name2, &cd1);
  fillChapterRandomly(testIndex);

  // Verify the chunks are in the index and not in the open chapter.
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd1, UDS_LOCATION_IN_DENSE);

  // Update chunk2, moving it to the open chapter.  Then fill the chapter
  updateChunk(testIndex, &name2, &cd1, &cd2);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_OPEN_CHAPTER);
  fillChapterRandomly(testIndex);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);

  // Delete chunk2.  Expect the stale chunk2 to survive.
  deleteChunk(testIndex, &name2, true);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectSurvivingChunk(testIndex, &name2, &cd1);

  // Rebuild the index from the volume file.  The deleted chunk comes back.
  testIndex = rebuildIndex(testIndex);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectSurvivingChunk(testIndex, &name2, &cd2);
}

/**********************************************************************/
static void lazarusTest(void)
{
  // Insert two colliding chunks into the open chapter, and then fill the
  // chapter.
  struct uds_record_name name1, name2, name3;
  insertRandomChunk(testIndex, &name1, &cd1);
  insertCollidingChunk(testIndex, &name1, &name2, &cd1);
  fillChapterRandomly(testIndex); // close chapter 0 -- with chunks 1 and 2

  // Verify the chunks are in the index but not in the open chapter.
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd1, UDS_LOCATION_IN_DENSE);

  // Update name2, moving it to the open chapter.  Add another colliding
  // chunk, and then fill the chapter.
  updateChunk(testIndex, &name2, &cd1, &cd2);
  insertCollidingChunk(testIndex, &name1, &name3, &cd1);
  fillChapterRandomly(testIndex);  // close chapter 1 -- with chunks 2 and 3
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name3, &cd1, UDS_LOCATION_IN_DENSE);

  // Delete name3, expecting it to be gone.  Then fill the chapter.
  deleteChunk(testIndex, &name3, true);
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
  expectMissingChunk(testIndex, &name3);
  fillChapterRandomly(testIndex);  // close chapter 2
  expectChunk(testIndex, &name1, &cd1, UDS_LOCATION_IN_DENSE);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
  expectMissingChunk(testIndex, &name3);

  // Delete name1, expecting it to be gone. Expect name3 to be back
  // because name2 has the same address, is also in chapter 1, and is
  // not a collision record after name1 is deleted.
  deleteChunk(testIndex, &name1, true);
  expectMissingChunk(testIndex, &name1);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
  expectSurvivingChunk(testIndex, &name3, &cd1);

  // Rebuild the index from the volume file.  The deleted chunks come back.
  testIndex = rebuildIndex(testIndex);
  expectSurvivingChunk(testIndex, &name1, &cd1);
  expectChunk(testIndex, &name2, &cd2, UDS_LOCATION_IN_DENSE);
  expectSurvivingChunk(testIndex, &name3, &cd1);
}

/**********************************************************************/
static const CU_TestInfo indexTests[] = {
  {"Simple delete from Open Chapter",    simpleOpenTest },
  {"Three deletes in one Chapter",       threeDeletesTest },
  {"Simple delete from Closed Chapter",  simpleClosedTest },
  {"Delete collision in Closed Chapter", collisionClosedTest },
  {"Complex collision with a Lazarus",   lazarusTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "Index_t3",
  .initializerWithBlockDevice = initSuite,
  .cleaner                    = cleanSuite,
  .tests                      = indexTests, // List of suite tests
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
