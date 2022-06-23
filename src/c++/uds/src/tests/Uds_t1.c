// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * This suite includes tests of the block context interfaces using
 * struct uds_request.
 **/

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "request.h"
#include "testPrototypes.h"
#include "uds.h"

static struct uds_index_session *indexSession;

/**********************************************************************/
#define ASSERT_FIELDS_MATCH(T1, F1, T2, F2)            \
  do {                                                 \
    T1 s1;                                             \
    T2 s2;                                             \
    CU_ASSERT_EQUAL(sizeof(s1.F1), sizeof(s2.F2));     \
    CU_ASSERT_EQUAL(offsetof(T1,F1), offsetof(T2,F2)); \
  } while (0)

/**********************************************************************/
static void callback(struct uds_request *request)
{
  UDS_ASSERT_SUCCESS(request->status);
}

/**********************************************************************/
static void basicsTest(void)
{
  struct uds_request *request;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct uds_request, __func__, &request));
  UDS_ASSERT_ERROR(-EINVAL, uds_start_chunk_operation(request));
  request->callback = callback;

  struct uds_chunk_data meta1, meta2, meta3;
  createRandomMetadata(&meta1);
  createRandomMetadata(&meta2);
  createRandomMetadata(&meta3);

  request->session = indexSession;

  // First post - create new entry
  request->type = UDS_POST;
  request->found = true;
  request->new_metadata = meta1;
  request->old_metadata = meta3;
  createRandomBlockName(&request->chunk_name);
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_FALSE(request->found);

  // second post - find existing entry
  request->type = UDS_POST;
  request->found = false;
  request->new_metadata = meta2;
  request->old_metadata = meta3;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_TRUE(request->found);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->old_metadata, &meta1);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->new_metadata, &meta2);

  // query - find existing entry
  request->type = UDS_QUERY;
  request->found = false;
  request->new_metadata = meta3;
  request->old_metadata = meta3;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_TRUE(request->found);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->old_metadata, &meta1);

  // update - replace existing entry
  request->type = UDS_UPDATE;
  request->found = false;
  request->new_metadata = meta2;
  request->old_metadata = meta3;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_TRUE(request->found);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->old_metadata, &meta1);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->new_metadata, &meta2);

  // query - find newer entry
  request->type = UDS_QUERY;
  request->found = false;
  request->new_metadata = meta3;
  request->old_metadata = meta3;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_TRUE(request->found);
  UDS_ASSERT_BLOCKDATA_EQUAL(&request->old_metadata, &meta2);

  // delete - delete existing entry
  request->type = UDS_DELETE;
  request->found = false;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_TRUE(request->found);

  // query - find no entry
  request->type = UDS_QUERY;
  request->found = false;
  request->new_metadata = meta3;
  request->old_metadata = meta3;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_FALSE(request->found);

  // delete - delete nonexisting entry
  request->type = UDS_DELETE;
  request->found = true;
  UDS_ASSERT_SUCCESS(uds_start_chunk_operation(request));
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
  CU_ASSERT_FALSE(request->found);

  // Index statistics
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(indexStats.collisions,          0);
  CU_ASSERT_EQUAL(indexStats.entries_discarded,   1);
  CU_ASSERT_EQUAL(indexStats.entries_indexed,     0);
  CU_ASSERT_EQUAL(indexStats.deletions_found,     1);
  CU_ASSERT_EQUAL(indexStats.deletions_not_found, 1);
  CU_ASSERT_EQUAL(indexStats.posts_found,         1);
  CU_ASSERT_EQUAL(indexStats.posts_not_found,     1);
  CU_ASSERT_EQUAL(indexStats.queries_found,       2);
  CU_ASSERT_EQUAL(indexStats.queries_not_found,   1);
  CU_ASSERT_EQUAL(indexStats.updates_found,       1);
  CU_ASSERT_EQUAL(indexStats.updates_not_found,   0);
  CU_ASSERT_EQUAL(indexStats.requests,            8);

  UDS_FREE(request);
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"uds_request basics", basicsTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                   = "Uds_t1",
  .initializerWithSession = initializerWithSession,
  .tests                  = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
