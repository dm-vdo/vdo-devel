// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/atomic.h>

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "uds-threads.h"
#include "uds.h"

static const char *indexName;
static struct uds_parameters params;
static struct uds_index_session *indexSession;

enum { NUM_CHAPTERS = 10 };

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
static void setupIndexAndSession(int startingChapters, bool save)
{
  initializeOldInterfaces(2000);

  // Create a new index.
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));

  // Fill the requested number of chapters.
  unsigned long blockCount
    = startingChapters * getBlocksPerChapter(indexSession);
  postChunks(indexSession, 0, blockCount, UDS_SUCCESS);
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(blockCount, indexStats.entries_indexed);
  CU_ASSERT_EQUAL(0, indexStats.posts_found);
  CU_ASSERT_EQUAL(blockCount, indexStats.posts_not_found);
  if (save) {
    UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
    UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  }
}

/**********************************************************************/
static void teardownIndexAndSession(void)
{
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void suspendNoIndexTest(void)
{
  setupIndexAndSession(0, true);
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));

  // Resuming when not suspended just succeeds.
  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, indexName));
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));

  // We can't create or load an index while suspended.
  UDS_ASSERT_ERROR(-EBUSY, uds_open_index(UDS_CREATE, &params, indexSession));
  UDS_ASSERT_ERROR(-EBUSY, uds_open_index(UDS_LOAD, &params, indexSession));

  // Suspending when already suspended also just succeeds.
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));
  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, indexName));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void suspendIndexTest(void)
{
  setupIndexAndSession(0, true);
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));

  postChunks(indexSession, 0, 1, UDS_SUCCESS);
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));
  postChunks(indexSession, 1, 1, -EBUSY);
  UDS_ASSERT_ERROR(-EBUSY, uds_close_index(indexSession));

  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, NULL));
  postChunks(indexSession, 1, 1, UDS_SUCCESS);
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));
  postChunks(indexSession, 2, 1, -EBUSY);

  // This will lose the unsaved index state.
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));

  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(0, indexStats.entries_indexed);

  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void suspendSaveIndexTest(void)
{
  setupIndexAndSession(0, true);
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));

  postChunks(indexSession, 0, 1, UDS_SUCCESS);
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, true));
  postChunks(indexSession, 1, 1, -EBUSY);
  UDS_ASSERT_ERROR(-EBUSY, uds_close_index(indexSession));

  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, NULL));
  postChunks(indexSession, 1, 1, UDS_SUCCESS);
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, true));
  postChunks(indexSession, 2, 1, -EBUSY);

  // The index state will be saved.
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));

  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, indexSession));
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(2, indexStats.entries_indexed);

  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void rebuildThread(void *arg)
{
  int expectedResult = *((int *) arg);
  UDS_ASSERT_ERROR(expectedResult,
                   uds_open_index(UDS_LOAD, &params, indexSession));
}

/**********************************************************************/
static void suspendRebuildTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, true);

  /*
   * At this point we have a saved volume containing several chapters.
   * Discard the index state so that we need to do a full rebuild (using index
   * interfaces).
   */
  struct configuration *tempConfig;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &tempConfig));
  tempConfig->zone_count = 1;
  struct uds_index *index;
  UDS_ASSERT_SUCCESS(make_index(tempConfig, UDS_NO_REBUILD, NULL, NULL,
                                &index));
  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  free_index(index);
  uds_free_configuration(tempConfig);

  // Make sure the index will not load.
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_ERROR(-EEXIST,
                   uds_open_index(UDS_NO_REBUILD, &params, indexSession));

  // Rebuild the index in a separate thread so we can suspend and stop it.
  int startChapters = atomic_read_acquire(&chapters_replayed);
  int expectedRebuildResult = -EBUSY;
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(rebuildThread, &expectedRebuildResult,
                                       "suspend", &thread));

  // Wait for the rebuild to start.
  while (startChapters == atomic_read_acquire(&chapters_replayed)) {
    sleep_for(ms_to_ktime(10));
  }

  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));
  int suspendChapters = atomic_read_acquire(&chapters_replayed);
  CU_ASSERT((suspendChapters - startChapters) < NUM_CHAPTERS);
  for (int i = 0; i < 10; i++) {
    sleep_for(ms_to_ktime(25));
    if (suspendChapters == atomic_read_acquire(&chapters_replayed)) {
      break;
    }
  }
  int suspendChapters2 = atomic_read_acquire(&chapters_replayed);
  CU_ASSERT_EQUAL(suspendChapters, suspendChapters2);

  // Shut down the suspended index session, discarding rebuild progress.
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uds_join_threads(thread);

  int closeChapters = atomic_read_acquire(&chapters_replayed);
  CU_ASSERT_EQUAL(suspendChapters, closeChapters);

  // Make sure the rebuild did not succeed, and the index still will not load.
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_ERROR(-EEXIST,
                   uds_open_index(UDS_NO_REBUILD, &params, indexSession));

  // Rebuild the index in a separate thread so we can suspend and resume it.
  startChapters = atomic_read_acquire(&chapters_replayed);
  expectedRebuildResult = UDS_SUCCESS;
  UDS_ASSERT_SUCCESS(uds_create_thread(rebuildThread, &expectedRebuildResult,
                                       "suspend", &thread));

  // Wait for the replay to start.
  while (startChapters == atomic_read_acquire(&chapters_replayed)) {
    sleep_for(ms_to_ktime(10));
  }
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, false));

  suspendChapters = atomic_read_acquire(&chapters_replayed);
  CU_ASSERT((suspendChapters - startChapters) < NUM_CHAPTERS);
  for (int i = 0; i < 10; i++) {
    sleep_for(ms_to_ktime(25));
    if (suspendChapters == atomic_read_acquire(&chapters_replayed)) {
      break;
    }
  }
  suspendChapters2 = atomic_read_acquire(&chapters_replayed);
  CU_ASSERT_EQUAL(suspendChapters, suspendChapters2);
  UDS_ASSERT_SUCCESS(uds_resume_index_session(indexSession, NULL));
  uds_join_threads(thread);

  // Check that the rebuild succeeded.
  // Rewrite the first N-1 chapters of chunks to show they're all in
  // the index. If the index uses more than one zone, some chunks may
  // spill over into the open chapter and not get rebuilt.
  unsigned long blocksToCheck
    = (NUM_CHAPTERS - 1) * getBlocksPerChapter(indexSession);
  postChunks(indexSession, 0, blocksToCheck, UDS_SUCCESS);
  struct uds_index_stats indexStats;
  UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &indexStats));
  CU_ASSERT_EQUAL(blocksToCheck, indexStats.posts_found);
  CU_ASSERT_EQUAL(0, indexStats.posts_not_found);
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void closeThread(void *arg __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
}

/**********************************************************************/
static void suspendThread(void *arg __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, true));
}

/**********************************************************************/
static void destroyThread(void *arg __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
}

/**********************************************************************/
static void suspendSuspendTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a suspend operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(suspendThread, NULL, "suspend",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch another suspend with a save.
  UDS_ASSERT_ERROR(-EBUSY, uds_suspend_index_session(indexSession, true));
  uds_join_threads(thread);

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void suspendCloseTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a suspend operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(suspendThread, NULL, "suspend",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a close.
  UDS_ASSERT_ERROR(-EBUSY, uds_close_index(indexSession));
  uds_join_threads(thread);

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void suspendDestroyTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a suspend operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(suspendThread, NULL, "suspend",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a destroy.
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uds_join_threads(thread);

  teardownIndexAndSession();
}

/**********************************************************************/
static void closeSuspendTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch the close operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(closeThread, NULL, "close", &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a suspend with a save.
  UDS_ASSERT_SUCCESS(uds_suspend_index_session(indexSession, true));
  uds_join_threads(thread);

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void closeCloseTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch the close operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(closeThread, NULL, "close", &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch another close.
  UDS_ASSERT_ERROR(-ENOENT, uds_close_index(indexSession));
  uds_join_threads(thread);

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  teardownIndexAndSession();
}

/**********************************************************************/
static void closeDestroyTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch the close operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(closeThread, NULL, "close", &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a destroy.
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uds_join_threads(thread);

  teardownIndexAndSession();
}

/**********************************************************************/
static void destroySuspendTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a destroy operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(destroyThread, NULL, "destroy",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a suspend with a save.
  UDS_ASSERT_ERROR(-EBUSY, uds_suspend_index_session(indexSession, true));
  uds_join_threads(thread);

  teardownIndexAndSession();
}

/**********************************************************************/
static void destroyCloseTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a destroy operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(destroyThread, NULL, "destroy",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch a close.
  UDS_ASSERT_ERROR(-ENOENT, uds_close_index(indexSession));
  uds_join_threads(thread);

  teardownIndexAndSession();
}

/**********************************************************************/
static void destroyDestroyTest(void)
{
  setupIndexAndSession(NUM_CHAPTERS, false);

  // Launch a destroy operation to start a save.
  int startChapters = atomic_read_acquire(&saves_begun);
  struct thread *thread;
  UDS_ASSERT_SUCCESS(uds_create_thread(destroyThread, NULL, "destroy",
                                       &thread));
  while (startChapters == atomic_read_acquire(&saves_begun)) {
    sleep_for(ms_to_ktime(10));
  }

  // While the first save is running, launch another destroy.
  UDS_ASSERT_ERROR(-EBUSY, uds_destroy_index_session(indexSession));
  uds_join_threads(thread);

  teardownIndexAndSession();
}

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  indexName = name;
  struct uds_parameters parameters = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  randomizeUdsNonce(&parameters);
  params = parameters;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"Suspend with no index",  suspendNoIndexTest   },
  {"Suspend with index",     suspendIndexTest     },
  {"Suspend with save",      suspendSaveIndexTest },
  {"Suspend during rebuild", suspendRebuildTest   },
  {"Suspend during suspend", suspendSuspendTest   },
  {"Close during suspend",   suspendCloseTest     },
  {"Destroy during suspend", suspendDestroyTest   },
  {"Suspend during close",   closeSuspendTest     },
  {"Close during close",     closeCloseTest       },
  {"Destroy during close",   closeDestroyTest     },
  {"Suspend during destroy", destroySuspendTest   },
  {"Close during destroy",   destroyCloseTest     },
  {"Destroy during destroy", destroyDestroyTest   },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Suspend_t1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
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
