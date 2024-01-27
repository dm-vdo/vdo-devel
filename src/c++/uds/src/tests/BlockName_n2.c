// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * BlockName_n2 tests the uds_launch_request interface, using datasets
 * large enough to force chapters to be read back in from volume storage.
 **/

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "index.h"
#include "index-session.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

typedef struct {
  struct uds_parameters     parameters;
  struct uds_index_session *indexSession;
} TestIndex;

typedef struct {
  int startCounter;
  int numChunks;
  enum uds_request_type type;
  struct uds_record_data newMetadata;
  struct uds_record_data oldMetadata;
  bool isIndexed;
  bool isSparse;
} Group;

typedef struct {
  Group *group;
  struct uds_request request;
} GroupRequest;

typedef struct {
  uint64_t postsFound;
  uint64_t postsNotFound;
  uint64_t queriesFound;
  uint64_t queriesNotFound;
  uint64_t updatesFound;
  uint64_t updatesNotFound;
} ExpectStats;

static TestIndex globalTestIndex;
static int       divisor;
static bool      reopenFlag;
static bool      suspendFlag;

/**********************************************************************/
static void callback(struct uds_request *request)
{
  GroupRequest *groupRequest = container_of(request, GroupRequest, request);
  Group *group = groupRequest->group;
  UDS_ASSERT_SUCCESS(request->status);
  CU_ASSERT_EQUAL(request->type, group->type);
  UDS_ASSERT_EQUAL_BYTES(&request->new_metadata, &group->newMetadata,
                         sizeof(struct uds_record_data));
  if (!group->isSparse) {
    if (group->isIndexed) {
      CU_ASSERT_TRUE(request->found);
      UDS_ASSERT_EQUAL_BYTES(&request->old_metadata, &group->oldMetadata,
                             sizeof(struct uds_record_data));
    } else {
      CU_ASSERT_FALSE(request->found);
    }
  }
}

/**********************************************************************/
static void setExpectations(struct uds_index_session *indexSession,
                            ExpectStats              *expect)
{
  struct uds_index_stats stats;
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &stats));
  expect->postsFound      = stats.posts_found;
  expect->postsNotFound   = stats.posts_not_found;
  expect->queriesFound    = stats.queries_found;
  expect->queriesNotFound = stats.queries_not_found;
  expect->updatesFound    = stats.updates_found;
  expect->updatesNotFound = stats.updates_not_found;
}

/**********************************************************************/
static void checkExpectations(struct uds_index_session *indexSession,
                              ExpectStats              *expect)
{
  struct uds_index_stats stats;
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &stats));
  if (isIndexSparse(indexSession)) {
    CU_ASSERT(stats.posts_found   <= expect->postsFound);
    CU_ASSERT(stats.queries_found <= expect->queriesFound);
    CU_ASSERT(stats.posts_found + stats.posts_not_found
              == expect->postsFound + expect->postsNotFound);
    CU_ASSERT(stats.queries_found + stats.queries_not_found
              == expect->queriesFound + expect->queriesNotFound);
  } else {
    CU_ASSERT(stats.posts_found      == expect->postsFound);
    CU_ASSERT(stats.posts_not_found   == expect->postsNotFound);
    CU_ASSERT(stats.queries_found    == expect->queriesFound);
    CU_ASSERT(stats.queries_not_found == expect->queriesNotFound);
  }
  CU_ASSERT(stats.updates_found    == expect->updatesFound);
  CU_ASSERT(stats.updates_not_found == expect->updatesNotFound);
}

/**********************************************************************/
static void doGroup(TestIndex *testIndex, Group *group,
                    enum uds_request_type type)
{
  ExpectStats expect;
  setExpectations(testIndex->indexSession, &expect);
  uint64_t counter = group->startCounter;
  struct uds_record_data metadata;
  get_random_bytes(&metadata, sizeof(metadata));
  group->type        = type;
  group->newMetadata = metadata;
  GroupRequest *groupRequests;
  UDS_ASSERT_SUCCESS(uds_allocate(group->numChunks, GroupRequest, __func__,
                                  &groupRequests));
  int n;
  for (n = 0; n < group->numChunks; n++) {
    GroupRequest *gr = &groupRequests[n];
    gr->group                = group;
    gr->request.callback     = callback;
    gr->request.record_name  = hash_record_name(&counter, sizeof(counter));
    gr->request.session      = testIndex->indexSession;
    gr->request.new_metadata = group->newMetadata;
    gr->request.type         = type;
    counter++;
    UDS_ASSERT_SUCCESS(uds_launch_request(&gr->request));
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(testIndex->indexSession));
  uds_free(groupRequests);
  switch (type) {
  default:
    CU_FAIL("Unknown type");
    break;
  case UDS_POST:
    if (group->isIndexed) {
      expect.postsFound += group->numChunks;
    } else {
      expect.postsNotFound += group->numChunks;
    }
    break;
  case UDS_QUERY:
  case UDS_QUERY_NO_UPDATE:
    if (group->isIndexed) {
      expect.queriesFound += group->numChunks;
    } else {
      expect.queriesNotFound += group->numChunks;
    }
    break;
  case UDS_UPDATE:
    if (group->isIndexed) {
      expect.updatesFound += group->numChunks;
    } else {
      expect.updatesNotFound += group->numChunks;
    }
    break;
  }
  checkExpectations(testIndex->indexSession, &expect);
  if (((type == UDS_POST) && !group->isIndexed) || (type == UDS_UPDATE)) {
    group->oldMetadata = metadata;
    group->isIndexed   = true;
  }
}

/**********************************************************************/
static void modifySessionConfiguration(struct uds_index_session *indexSession,
                                       bool create)
{
  struct uds_parameters *params;
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(indexSession, &params));
  if (params->sparse) {
    struct uds_configuration *config;
    UDS_ASSERT_SUCCESS(uds_make_configuration(params, &config));
    unsigned int chapters_per_volume = config->geometry->chapters_per_volume;
    resizeSparseConfiguration(config, 0, 0, 0, chapters_per_volume - 2, 0);
    config->cache_chapters = 3;

    // Remake the index with the modified configuration.
    struct uds_index *oldIndex = indexSession->index;
    struct uds_index *newIndex;
    enum uds_open_index_type openType = (create ? UDS_CREATE : UDS_NO_REBUILD);
    UDS_ASSERT_SUCCESS(uds_save_index(oldIndex));
    UDS_ASSERT_SUCCESS(uds_make_index(config,
                                      openType,
                                      oldIndex->load_context,
                                      oldIndex->callback,
                                      &newIndex));
    indexSession->index = newIndex;
    uds_free_index(oldIndex);
    uds_free_configuration(config);
  }
  uds_free(params);
}

/**********************************************************************/
static void newSection(TestIndex *testIndex)
{
  // Begin a new section of the test. The values of reopenFlag and suspendFlag
  // control whether or not we close or suspend the index before continuing.

  struct uds_index_session *session = testIndex->indexSession;
  if (reopenFlag) {
    // The point of this is to empty the volume cache and therefore force
    // the reading of the closed chapters.
    struct uds_parameters *oldParams;
    UDS_ASSERT_SUCCESS(uds_get_index_parameters(session, &oldParams));
    if (oldParams->sparse) {
      // If the index is sparse, we can't reopen the index with oldConfig.
      // Instead, save and replace the index directly.
      modifySessionConfiguration(session, false);
    } else {
      UDS_ASSERT_SUCCESS(uds_close_index(session));
      UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, oldParams, session));
    }
    uds_free(oldParams);
  }
  if (suspendFlag) {
    // The point of this is to demonstrate that inserting a suspend and
    // resume does not affect the use of the index.
    UDS_ASSERT_SUCCESS(uds_suspend_index_session(session, true));
    UDS_ASSERT_SUCCESS(uds_resume_index_session(session, NULL));
  }
}

/**********************************************************************/
static void runTest(TestIndex *testIndex)
{
  int chunksPerGroup = getBlocksPerChapter(testIndex->indexSession) / divisor;
  bool isSparse = isIndexSparse(testIndex->indexSession);

  enum {
    NG1 = 13,
    NG2 = 17,
    NG3 = 19,
    NUM_GROUPS = 23
  };
  Group *groups;
  UDS_ASSERT_SUCCESS(uds_allocate(NUM_GROUPS, Group, __func__, &groups));
  int g;
  for (g = 0; g < NUM_GROUPS; g++) {
    groups[g].startCounter = g * chunksPerGroup;
    groups[g].numChunks    = chunksPerGroup;
    groups[g].isIndexed    = false;
    groups[g].isSparse     = isSparse;
  }

  // This loop posts a lot of new groups into the empty index
  albPrint("Posting %d groups of %d chunks", NG1, chunksPerGroup);
  for (g = 0; g < NG1; g++) {
    doGroup(testIndex, &groups[g], UDS_POST);
  }

  newSection(testIndex);

  // This loop queries all the groups.
  albPrint("Querying %d groups of %d chunks", NUM_GROUPS, chunksPerGroup);
  for (g = 0; g < NUM_GROUPS; g++) {
    doGroup(testIndex, &groups[2 * g % NUM_GROUPS], UDS_QUERY);
  }

  newSection(testIndex);

  // This loop posts the groups again and checks that we find the metadata
  // from the first posting.  Also add some new groups.
  albPrint("Posting %d groups of %d chunks", NG2, chunksPerGroup);
  for (g = 0; g < NG2; g++) {
    doGroup(testIndex, &groups[3 * g % NG2], UDS_POST);
  }

  newSection(testIndex);

  // This loop queries all the groups.
  albPrint("Querying %d groups of %d chunks", NUM_GROUPS, chunksPerGroup);
  for (g = 0; g < NUM_GROUPS; g++) {
    doGroup(testIndex, &groups[4 * g % NUM_GROUPS], UDS_QUERY_NO_UPDATE);
  }

  newSection(testIndex);

  // This loop posts the groups again and checks that we find the metadata
  // from the first posting.  Also add some new groups.
  albPrint("Posting %d groups of %d chunks", NG3, chunksPerGroup);
  for (g = 0; g < NG3; g++) {
    doGroup(testIndex, &groups[5 * g % NG3], UDS_POST);
  }

  newSection(testIndex);

  // This loop updates all the groups.
  albPrint("Updating %d groups of %d chunks", NUM_GROUPS, chunksPerGroup);
  for (g = 0; g < NUM_GROUPS; g++) {
    doGroup(testIndex, &groups[6 * g % NUM_GROUPS], UDS_QUERY);
  }

  newSection(testIndex);

  // This loop queries all the groups.
  albPrint("Querying %d groups of %d chunks", NUM_GROUPS, chunksPerGroup);
  for (g = 0; g < NUM_GROUPS; g++) {
    doGroup(testIndex, &groups[7 * g % NUM_GROUPS], UDS_QUERY_NO_UPDATE);
  }

  uds_free(groups);
}

/**********************************************************************/
static void multiTestWorker(void *argument)
{
  TestIndex *testIndex = (TestIndex *) argument;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&testIndex->indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &testIndex->parameters,
                                    testIndex->indexSession));
  runTest(testIndex);
  UDS_ASSERT_SUCCESS(uds_close_index(testIndex->indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(testIndex->indexSession));
}

/**********************************************************************/
static void runMultiTest(int testDivisor)
{
  divisor = testDivisor;

  struct block_device *const *testDevices = getTestMultiBlockDevices();

  enum { INDEX_COUNT = 2 };
  TestIndex ti[INDEX_COUNT];
  struct thread *threads[INDEX_COUNT];
  int i;
  for (i = 0; i < INDEX_COUNT; i++) {
    struct uds_parameters params = {
      .memory_size = UDS_MEMORY_CONFIG_256MB,
      .bdev = testDevices[i],
    };
    ti[i].parameters = params;
    randomizeUdsNonce(&ti[i].parameters);
    UDS_ASSERT_SUCCESS(uds_create_thread(multiTestWorker, &ti[i], "BNn2",
                                         &threads[i]));
  }
  for (i = 0; i < INDEX_COUNT; i++) {
    UDS_ASSERT_SUCCESS(uds_join_threads(threads[i]));
    putTestBlockDevice(testDevices[i]);
  }
}

/**********************************************************************/
static void oneChapterTest(void)
{
  // Run the test with the size of a group at 1/32 of a chapter.  Since we
  // write 23 groups, all the record names will fit in the open chapter.
  divisor = 32;
  runTest(&globalTestIndex);
}

/**********************************************************************/
static void manyChapterTest(void)
{
  // Run the test with the size of a group at 1/2 of a chapter.  We will
  // use 10+ chapters of record names, and will cycle through many chapters.
  divisor = 2;
  runTest(&globalTestIndex);
}

/**********************************************************************/
static void multiIndexOneChapterTest(void)
{
  // Run the test with the size of a group at 1/32 of a chapter.  Since we
  // write 23 groups, all the record names will fit in the open chapter.
  runMultiTest(32);
}

/**********************************************************************/
static void multiIndexManyChapterTest(void)
{
  // Run the test with the size of a group at 1/2 of a chapter.  We will
  // use 10+ chapters of record names, and will cycle through many chapters.
  runMultiTest(2);
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  globalTestIndex.indexSession = is;
  modifySessionConfiguration(globalTestIndex.indexSession, true);
}

/**********************************************************************/
static void initializerBasic(void)
{
  reopenFlag  = false;
  suspendFlag = false;
}

/**********************************************************************/
static void initializerLoad(void)
{
  // Do a save/load operation between sections.  This will test that we are
  // reading the closed chapters.
  reopenFlag  = true;
  suspendFlag = false;
}

/**********************************************************************/
static void initializerSuspend(void)
{
  // Do a suspend/resume operation between sections.  This will test that
  // suspending the index leaves everything operable.
  reopenFlag  = false;
  suspendFlag = true;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "one chapter",  oneChapterTest },
  { "many chapter", manyChapterTest },
  CU_TEST_INFO_NULL,
};

static const CU_TestInfo multiTests[] = {
  { "multi index one chapter",  multiIndexOneChapterTest },
  { "multi index many chapter", multiIndexManyChapterTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suites[] = {
  {
    .name                     = "BlockName_n2.basic",
    .initializerWithSession   = initializerWithSession,
    .initializer              = initializerBasic,
    .tests                    = tests,
    .next                     = &suites[1],
  },
  {
    .name                     = "BlockName_n2.suspend",
    .initializerWithSession   = initializerWithSession,
    .initializer              = initializerSuspend,
    .tests                    = tests,
    .next                     = &suites[2],
  },
  {
    .name                     = "BlockName_n2.load",
    .initializerWithSession   = initializerWithSession,
    .initializer              = initializerLoad,
    .tests                    = tests,
    .next                     = &suites[3],
  },
  {
    .name        = "BlockName_n2.multi.basic",
    .initializer = initializerBasic,
    .tests       = multiTests,
    .next        = &suites[4],
  },
  {
    .name        = "BlockName_n2.multi.suspend",
    .initializer = initializerSuspend,
    .tests       = multiTests,
    .next        = &suites[5],
  },
  {
    .name        = "BlockName_n2.multi.load",
    .initializer = initializerLoad,
    .tests       = multiTests,
  }
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return suites;
}
