// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * PostAndUpdate_x1 tests that we can load an index using udsPostBlockNames in
 * one thread while using udsUpdateBlockMapping in another thread.
 *
 * There are two distinct set of index accesses happening in this test: the
 * posts and the updates.  The posts are come from a single thread that is
 * hashing small blocks and calling udsPostBlockName to add the name to the
 * index.  The updates come from the Albireo callback thread saving the block
 * names on a funnel queue, and then another thread takes the block name off of
 * the funnel queue and calls udsUpdateBlockMapping.
 *
 * This can cause a problem when the posts get far ahead of the updates,
 * because the list of updates to do consumes a lot of memory.  On a small
 * system (like an AFARM) the system eventually starts paging memory to disk,
 * and then the update thread slows down because it is taking page faults.
 * When the system runs out of memory, the linux OOM-Killer kills the process.
 *
 * We resolve the difficulty by introducing the throttle code.  When the posts
 * get ahead of the updates by the numBlocksThreshold (which is 12 chapters of
 * block names), we sleep the post thread for 20 seconds and let the update
 * thread try to catch up.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "event-count.h"
#include "funnel-queue.h"
#include "hash-utils.h"
#include "indexer.h"
#include "memory-alloc.h"
#include "oldInterfaces.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "thread-utils.h"

typedef struct {
  struct funnel_queue_entry queueEntry;
  unsigned long    chunkCounter;
} TestBlockCounter;

enum { NUM_CHAPTERS_THRESHOLD = 12 };

static unsigned long              postCounter = 0;
static unsigned long              numBlocksInTest;
static unsigned long              numBlocksThreshold;
static struct event_count        *testEvent;
static struct funnel_queue       *testQueue;
static struct uds_index_session  *indexSession;

/**********************************************************************/
static void reportStats(void)
{
  struct uds_index_stats stats;
  UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &stats));
  albPrint("PostsFound: %llu", (unsigned long long) stats.posts_found);
  albPrint("PostsNotFound: %llu", (unsigned long long) stats.posts_not_found);
  albPrint("UpdatesFound: %llu", (unsigned long long) stats.updates_found);
  albPrint("UpdatesNotFound: %llu",
           (unsigned long long) stats.updates_not_found);
}

/**********************************************************************/
static void report(const char *label, unsigned long counter)
{
  albPrint("Launched %luM %s", counter >> 20, label);
  reportStats();
}

/**********************************************************************/
static void throttle(void)
{
  for (;;) {
    struct uds_index_stats stats;
    UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &stats));
    unsigned long posts = stats.posts_found + stats.posts_not_found;
    unsigned long updates = stats.updates_found + stats.updates_not_found;
    if (posts < updates + numBlocksThreshold) {
      return;
    }
    albPrint("Throttling oldPostBlockName");
    sleep_for(seconds_to_ktime(20));
  }
}

/**********************************************************************/
static void hashChunkCounter(struct uds_record_name *name,
                             unsigned long           counter)
{
  *name = hash_record_name(&counter, sizeof(counter));
}

/**********************************************************************/
static void cb(enum uds_request_type type,
               int status,
               OldCookie cookie,
               struct uds_record_data *duplicateAddress __attribute__((unused)),
               struct uds_record_data *canonicalAddress __attribute__((unused)),
               struct uds_record_name *blockName __attribute__((unused)),
               void *data __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(status);
  if (type == UDS_POST) {
    TestBlockCounter *tbc = cookie;
    uds_funnel_queue_put(testQueue, &tbc->queueEntry);
    event_count_broadcast(testEvent);
  }
}

/**********************************************************************/
static void updateBlockNames(void *argument)
{
  struct uds_index_session *indexSession = argument;
  unsigned long counter;
  for (counter = 0; counter < numBlocksInTest; ) {
    struct funnel_queue_entry *fqe;
    while ((fqe = uds_funnel_queue_poll(testQueue)) == NULL) {
      event_token_t token = event_count_prepare(testEvent);
      if ((fqe = uds_funnel_queue_poll(testQueue)) != NULL) {
        event_count_cancel(testEvent, token);
        break;
      }
      event_count_wait(testEvent, token, NULL);
    }
    TestBlockCounter *tbc = container_of(fqe, TestBlockCounter, queueEntry);
    struct uds_record_name chunkName;
    hashChunkCounter(&chunkName, tbc->chunkCounter);
    vdo_free(tbc);
    oldUpdateBlockMapping(indexSession, NULL, &chunkName,
                          (struct uds_record_data *) &chunkName, cb);
    counter++;
    if ((counter & ((1 << 23) - 1)) == 0) {
      report("oldUpdateBlockMapping", counter);
    }
  }
}

/**********************************************************************/
static void postBlockNames(struct uds_index_session *indexSession)
{
  unsigned long counter;
  for (counter = 0; counter < numBlocksInTest; ) {
    TestBlockCounter *tbc;
    UDS_ASSERT_SUCCESS(vdo_allocate(1, TestBlockCounter, __func__, &tbc));
    tbc->chunkCounter = postCounter++;
    struct uds_record_name chunkName;
    hashChunkCounter(&chunkName, tbc->chunkCounter);
    oldPostBlockName(indexSession, tbc, (struct uds_record_data *) &chunkName,
                     &chunkName, cb);
    counter++;
    if ((counter & ((1 << 23) - 1)) == 0) {
      report("oldPostBlockName", counter);
    }
    if ((counter & ((1 << 16) - 1)) == 0) {
      throttle();
    }
  }
}

/**********************************************************************/
static void postAndUpdate(void)
{
  struct uds_parameters *params;
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(indexSession, &params));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, indexSession));
  vdo_free(params);

  numBlocksInTest = getBlocksPerIndex(indexSession);
  numBlocksThreshold
    = NUM_CHAPTERS_THRESHOLD * getBlocksPerChapter(indexSession);

  struct thread *thread;
  UDS_ASSERT_SUCCESS(make_event_count(&testEvent));
  UDS_ASSERT_SUCCESS(uds_make_funnel_queue(&testQueue));
  UDS_ASSERT_SUCCESS(vdo_create_thread(updateBlockNames, indexSession, "updater",
                                       &thread));
  postBlockNames(indexSession);
  vdo_join_threads(thread);
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));

  free_event_count(testEvent);
  uds_free_funnel_queue(testQueue);

  reportStats();
}

/**********************************************************************/
static void postAndUpdateTest(void)
{
  enum { NUM_PASSES = 5 };
  initializeOldInterfaces(2000);
  int i;
  for (i = 0; i < NUM_PASSES; i++) {
    albPrint("===== Pass %d =====", i);
    postAndUpdate();
  }
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Post and Update", postAndUpdateTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "PostAndUpdate_x1",
  .initializerWithSession   = initializerWithSession,
  .oneIndexConfiguredByArgv = true,
  .tests                    = tests,
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
