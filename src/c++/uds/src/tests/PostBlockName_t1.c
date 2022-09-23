// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * PostBlock_t1 are basic tests of block mode.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "uds.h"

enum { NEW_CHUNK_COUNT    = 101 };
enum { REPEAT_CHUNK_COUNT = 53 };
enum { CHUNK_SIZE         = 4096 };

static struct uds_index_session *indexSession;

typedef struct {
  uint64_t entriesIndexed;
  uint64_t postsFound;
  uint64_t postsNotFound;
} Expectations;

/**********************************************************************/
static void assertExpectations(Expectations *expect)
{
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));

  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(expect->entriesIndexed, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(expect->postsFound, indexStats.posts_found);
  CU_ASSERT_EQUAL(expect->postsNotFound, indexStats.posts_not_found);
}

/**********************************************************************/
static void postBlockTest(void)
{
  Expectations expect;
  memset(&expect, 0, sizeof(expect));
  initializeOldInterfaces(1000);

  // Post some record names, and see that the stats are reported correctly
  // This is the Basic01 test rewritten to use udsPostBlockName.
  unsigned long counter;
  for (counter = 0; counter < NEW_CHUNK_COUNT; counter++) {
    struct uds_record_name chunkName
      = murmurHashChunkName(&counter, sizeof(counter), 0);
    oldPostBlockName(indexSession, NULL, (struct uds_record_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  expect.entriesIndexed += NEW_CHUNK_COUNT;
  expect.postsNotFound  += NEW_CHUNK_COUNT;
  assertExpectations(&expect);

  // Post some duplicate chunk again, and see that the stats are reported
  // correctly.  This is the Basic02 test rewritten to use
  // udsPostBlockName.
  for (counter = 0; counter < REPEAT_CHUNK_COUNT; counter++) {
    struct uds_record_name chunkName
      = murmurHashChunkName(&counter, sizeof(counter), 0);
    oldPostBlockName(indexSession, NULL, (struct uds_record_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  expect.postsFound += REPEAT_CHUNK_COUNT;
  assertExpectations(&expect);

  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Post Block", postBlockTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                   = "PostBlockName_t1",
  .initializerWithSession = initializerWithSession,
  .tests                  = tests,
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
