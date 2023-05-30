// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "testPrototypes.h"
#include "uds.h"

static const char *firstName;
static const char *secondName;
static struct uds_index_session *indexSession;

/**********************************************************************/
static void postChunks(struct uds_index_session *indexSession,
                       int                       base,
                       int                       count,
                       int                       expectedResult)
{
  long index;
  for (index = base; index < base + count; index++) {
    struct uds_record_name chunkName = hash_record_name(&index, sizeof(index));
    UDS_ASSERT_ERROR(expectedResult,
                     oldPostBlockNameResult(indexSession, NULL,
                                        (struct uds_record_data *) &chunkName,
                                        &chunkName, NULL));
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
}

/**********************************************************************/
static void swapStorage(bool save)
{
  const char *const *indexNames = getTestMultiIndexNames();
  struct uds_parameters firstParams = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexNames[0],
  };

  struct uds_parameters secondParams = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexNames[1],
  };

  firstName = indexNames[0];
  secondName = indexNames[1];

  initializeOldInterfaces(2000);
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &firstParams, indexSession));

  // Add some initial entries.
  unsigned long blockCount = 5 * getBlocksPerChapter(indexSession) / 2;
  postChunks(indexSession, 0, blockCount, UDS_SUCCESS);

  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(blockCount, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(0, indexStats.posts_found);
  CU_ASSERT_EQUAL(blockCount, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, save));

  // Copy index to the second device and resume. 
  uint64_t index_size;
  UDS_ASSERT_SUCCESS(uds_compute_index_size(&firstParams, &index_size));
  UDS_ASSERT_SUCCESS(copyDevice(firstName, secondName, index_size));
  UDS_ASSERT_ERROR2(-EIO, -ENOENT,
                    uds_resume_index_session(indexSession, "bogus-name"));
  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, secondName));
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(blockCount, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(0, indexStats.posts_found);
  CU_ASSERT_EQUAL(blockCount, indexStats.posts_not_found);

  // Verify old entries and add some new ones.
  postChunks(indexSession, 0, 2 * blockCount, UDS_SUCCESS);

  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(2 * blockCount, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(blockCount, indexStats.posts_found);
  CU_ASSERT_EQUAL(2 * blockCount, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  // Reopen the index at the new location to prove it persists.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &secondParams, indexSession));
  postChunks(indexSession, 0, 2 * blockCount, UDS_SUCCESS);
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(2 * blockCount, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(2 * blockCount, indexStats.posts_found);
  CU_ASSERT_EQUAL(0, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  if (save) {
    // If we didn't save before suspend, the old device may not be in a
    // loadable state. If it is, check that it got no new entries.
    UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &firstParams, indexSession));
    postChunks(indexSession, 0, blockCount, UDS_SUCCESS);
    UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &indexStats));
    CU_ASSERT_EQUAL(blockCount, indexStats.entries_indexed);
    CU_ASSERT_EQUAL(blockCount, indexStats.posts_found);
    UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  }

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void savedStorageTest(void)
{
  swapStorage(true);
}

/**********************************************************************/
static void unsavedStorageTest(void)
{
  swapStorage(false);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"Swap storage device (save)",    savedStorageTest },
  {"Swap storage device (no save)", unsavedStorageTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "Suspend_n2",
  .tests = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return  a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
