/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdarg.h>
#include <stdlib.h>

#include "albtest.h"
#include "memory-alloc.h"
#include "syscalls.h"

#include "block-map.h"
#include "completion.h"
#include "int-map.h"
#include "status-codes.h"
#include "vdo-page-cache.h"
#include "vio.h"
#include "wait-queue.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static const TestParameters defaultParameters = {
  .logicalBlocks  = 4096,
  .physicalBlocks = 1024,
  .slabSize       =   64,
  .noIndexRegion  = true,
};

typedef enum {
  WRITE_ERROR    = -5,
  PAGE_NEW       = 0,
  PAGE_WRITTEN   = 1,
  PAGE_REWRITTEN = 2,
} TestPageState;

typedef uint64_t CacheEntryUID;

static bool                     getRequested;
static bool                     readOnly;
static struct int_map          *pageMap;
static physical_block_number_t  maxPBN;
static struct thread_config    *threadConfig;
static sequence_number_t        period;
static struct vdo_page_cache   *cache;
static CacheEntryUID            nextCacheEntryUID = 0x1020304000000001UL;
static struct block_map_zone    zone;

typedef struct {
  sequence_number_t dirtyPeriod;
  TestPageState     state;
  uint64_t          uid;
} TestPageHeader;

enum {
  SMALL_CACHE_SIZE = 4,
  LARGE_CACHE_SIZE = 8,
  PAGE_DATA_SIZE   = VDO_BLOCK_SIZE - sizeof(TestPageHeader),
};

typedef struct {
  TestPageHeader header;
  byte           buffer[PAGE_DATA_SIZE];
} TestPage;

typedef struct {
  struct vdo_completion       completion;
  struct vdo_page_completion  pageCompletion;
  page_number_t               pageNumber;
  sequence_number_t           dirtyPeriod;
  bool                        writable;
  vdo_action                 *action;
} TestCompletion;

typedef struct {
  physical_block_number_t    pbn;
  uint16_t                   busyCount;
  enum vdo_page_buffer_state state;
  enum vdo_page_write_status writeStatus;
} PageCheck;

static PageCheck pageCheck;

/**********************************************************************/
static TestCompletion *asTestCompletion(struct vdo_completion *completion)
{
  TestCompletion *testCompletion = (TestCompletion *) completion;
  vdo_assert_completion_type(testCompletion->completion.type,
                             VDO_TEST_COMPLETION);
  return testCompletion;
}

/**********************************************************************/
static void initializeTestCompletion(TestCompletion *testCompletion)
{
  memset(testCompletion, 0, sizeof(TestCompletion));
  vdo_initialize_completion(&testCompletion->completion, vdo,
                            VDO_TEST_COMPLETION);
}

/**
 * Read hook to initialize new pages, and validate old ones.
 *
 * <p>Implements VDOPageReadFunction.
 **/
static int
validatePage(void                    *rawPage,
             physical_block_number_t  pbn,
             struct block_map_zone   *zone  __attribute__((unused)),
             void                    *pageContext)
{
  TestPage      *testPage   = rawPage;
  CacheEntryUID *uidContext = pageContext;

  if (int_map_get(pageMap, pbn) == NULL) {
    // This page has not been written out, so initialize it
    testPage->header.dirtyPeriod = 0;
    testPage->header.state       = PAGE_NEW;
    testPage->header.uid         = nextCacheEntryUID;
    *uidContext                  = nextCacheEntryUID++;
    memset(testPage->buffer, (byte) pbn, PAGE_DATA_SIZE);
    return VDO_SUCCESS;
  }

  return ((pbn == testPage->buffer[0]) ? VDO_SUCCESS : VDO_OUT_OF_RANGE);
}

/**
 * Write hook which confirms that pages are correctly written and that they
 * new pages get rewritten.
 *
 * <p>Implements VDOPageWriteFunction.
 **/
static bool checkPageWritten(void                  *rawPage,
                             struct block_map_zone *zone __attribute__((unused)),
                             void                  *pageContext)
{
  TestPage                *testPage   = rawPage;
  CacheEntryUID           *uidContext = pageContext;
  physical_block_number_t  pbn        = testPage->buffer[0];
  maxPBN                              = max(pbn, maxPBN);

  TestPage fromDisk;
  VDO_ASSERT_SUCCESS(layer->reader(layer, pbn, 1, (char *) &fromDisk));
  UDS_ASSERT_EQUAL_BYTES(testPage->buffer, fromDisk.buffer, PAGE_DATA_SIZE);
  CU_ASSERT_EQUAL(*uidContext, testPage->header.uid);
  testPage->header.dirtyPeriod = 0;

  void *marker;
  switch (testPage->header.state) {
  case PAGE_NEW:
    VDO_ASSERT_SUCCESS(int_map_put(pageMap, pbn, pageMap, true, &marker));
    CU_ASSERT_PTR_NULL(marker);
    testPage->header.state = PAGE_WRITTEN;
    return true;

  case PAGE_WRITTEN:
    VDO_ASSERT_SUCCESS(int_map_put(pageMap, pbn, cache, true, &marker));
    CU_ASSERT_PTR_EQUAL(pageMap, marker);
    testPage->header.state = PAGE_REWRITTEN;
    return false;

  case PAGE_REWRITTEN:
    CU_ASSERT_PTR_EQUAL(cache, int_map_get(pageMap, pbn));
    return false;

  default:
    CU_FAIL("Unknown page state: %d", testPage->header.state);
  }
}

/**********************************************************************/
static void allowEnteringAction(struct vdo_completion *completion)
{
  vdo_allow_read_only_mode_entry(zone.read_only_notifier, completion);
}

/**
 * Initialize test.
 *
 * @param cacheSize   The number of pages in the cache
 * @param maximumAge  The maximum age of a dirty page
 **/
static void initialize(page_count_t cacheSize, sequence_number_t maximumAge)
{
  initializeBasicTest(&defaultParameters);
  VDO_ASSERT_SUCCESS(make_int_map(cacheSize, 0, &pageMap));
  threadConfig = makeOneThreadConfig();
  VDO_ASSERT_SUCCESS(vdo_make_read_only_notifier(false,
                                                 threadConfig,
                                                 vdo,
                                                 &zone.read_only_notifier));
  performSuccessfulAction(allowEnteringAction);
  zone.state = (struct admin_state) {
    .current_state = VDO_ADMIN_STATE_NORMAL_OPERATION,
    .waiter = NULL,
  };
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo,
                                   0,
                                   0,
                                   VIO_TYPE_TEST,
                                   VIO_PRIORITY_METADATA,
                                   &zone.tree_zone,
                                   &zone.tree_zone.vio_pool));
  VDO_ASSERT_SUCCESS(vdo_make_page_cache(vdo,
                                         cacheSize,
                                         validatePage,
                                         checkPageWritten,
                                         sizeof(CacheEntryUID),
                                         maximumAge,
                                         &zone,
                                         &cache));
  zone.page_cache         = cache;
  zone.tree_zone.map_zone = &zone;
  VDO_ASSERT_SUCCESS(vdo_make_dirty_lists(4, NULL, &zone.tree_zone,
                                          &zone.tree_zone.dirty_lists));

  vdo_set_page_cache_initial_period(cache, 1);
  period = 1;
  maxPBN = 0;
}

/**
 * Default initialization, no hooks, small cache.
 **/
static void initializeWithDefaults(void)
{
  initialize(SMALL_CACHE_SIZE, 1);
}

/**
 * Finalize test.
 **/
static void finishVDOPageCacheT1(void)
{
  for (physical_block_number_t pbn = 0; pbn <= maxPBN; pbn++) {
    void *marker = int_map_remove(pageMap, pbn);
    CU_ASSERT_FALSE(marker == pageMap);
  }

  free_vio_pool(UDS_FORGET(zone.tree_zone.vio_pool));
  UDS_FREE(UDS_FORGET(zone.tree_zone.dirty_lists));
  vdo_free_page_cache(UDS_FORGET(cache));
  vdo_free_read_only_notifier(UDS_FORGET(zone.read_only_notifier));
  vdo_free_thread_config(UDS_FORGET(threadConfig));
  free_int_map(UDS_FORGET(pageMap));
  tearDownVDOTest();
}

/**********************************************************************/
static void awaitSuccessfulCompletion(TestCompletion *testComp)
{
  VDO_ASSERT_SUCCESS(awaitCompletion(&testComp->completion));
}

/**********************************************************************/
static void pageAction(struct vdo_completion *completion)
{
  TestCompletion *testCompletion = asTestCompletion(completion);
  struct vdo_completion  *pageCompletion
    = &testCompletion->pageCompletion.completion;
  testCompletion->action(pageCompletion);
  vdo_finish_completion(completion, pageCompletion->result);
}

/**
 * Action to mark a page dirty.
 *
 * @param completion  The page completion to mark dirty
 **/
static void markPageDirty(struct vdo_completion *completion)
{
  TestPage      *page       = vdo_dereference_writable_page(completion);
  CacheEntryUID *uidContext = vdo_get_page_completion_context(completion);
  CU_ASSERT_EQUAL(*uidContext, page->header.uid);

  sequence_number_t oldDirtyPeriod = page->header.dirtyPeriod;
  page->header.dirtyPeriod = asTestCompletion(completion->parent)->dirtyPeriod;
  vdo_mark_completed_page_dirty(completion, oldDirtyPeriod,
                                page->header.dirtyPeriod);
}

/**********************************************************************/
static int performPageAction(TestCompletion *testCompletion,
                             vdo_action     *action)
{
  testCompletion->action    = action;
  struct vdo_completion *completion = &testCompletion->completion;
  vdo_reset_completion(completion);
  return performAction(pageAction, completion);
}

/**
 * Fill an entire page with a single character and mark the page dirty
 *
 * @param testCompletion  the completion which holds the page
 * @param mark            the character with which to fill
 * @param dirtyPeriod     the period in which the page is dirtied
 **/
static void fillPage(TestCompletion    *testCompletion,
                     char               mark,
                     sequence_number_t  dirtyPeriod)
{
  testCompletion->dirtyPeriod = dirtyPeriod;
  struct vdo_completion *pageCompletion
    = &testCompletion->pageCompletion.completion;
  TestPage      *page         = vdo_dereference_writable_page(pageCompletion);
  CacheEntryUID *uidContext   = vdo_get_page_completion_context(pageCompletion);
  CU_ASSERT_EQUAL(*uidContext, page->header.uid);

  memset(page->buffer, mark, PAGE_DATA_SIZE);
  performPageAction(testCompletion, markPageDirty);
}

/**********************************************************************/
static void finishGettingPage(struct vdo_completion *completion)
{
  TestCompletion *testCompletion = completion->parent;
  if (testCompletion->action) {
    testCompletion->action(&testCompletion->completion);
  }
  vdo_finish_completion(&testCompletion->completion, completion->result);
}

/**********************************************************************/
static void getVDOPageAction(struct vdo_completion *completion)
{
  TestCompletion             *testCompletion = asTestCompletion(completion);
  struct vdo_page_completion *pageCompletion = &testCompletion->pageCompletion;
  vdo_init_page_completion(pageCompletion, cache, testCompletion->pageNumber,
                           testCompletion->writable, NULL, NULL, NULL);
  vdo_set_completion_callback_with_parent(&pageCompletion->completion,
                                          finishGettingPage,
                                          completion->callback_thread_id,
                                          completion);
  vdo_get_page(&pageCompletion->completion);
  signalState(&getRequested);
}

/**********************************************************************/
static void launchPageGet(page_number_t          pageNumber,
                          bool                   writable,
                          TestCompletion        *testCompletion,
                          vdo_action            *action)
{
  vdo_reset_completion(&testCompletion->completion);
  testCompletion->pageNumber = pageNumber;
  testCompletion->writable   = writable;
  testCompletion->action     = action;
  launchAction(getVDOPageAction, &testCompletion->completion);
}

/**********************************************************************/
static void getReadablePage(page_number_t   pageNumber,
                            TestCompletion *testCompletion)
{
  launchPageGet(pageNumber, false, testCompletion, NULL);
  awaitSuccessfulCompletion(testCompletion);
}

/**********************************************************************/
static void getWritablePage(page_number_t   pageNumber,
                            TestCompletion *testCompletion)
{
  launchPageGet(pageNumber, true, testCompletion, NULL);
  awaitSuccessfulCompletion(testCompletion);
}

/**
 * Initiate a drain of the page cache.
 *
 * Implements vdo_admin_initiator
 **/
static void initiateDrain(struct admin_state *state __attribute__((unused)))
{
  vdo_drain_page_cache(cache);
  vdo_block_map_check_for_drain_complete(&zone);
}

/**
 * An action wrapper for flushVDOPageCacheAsync().
 **/
static void flushCacheAction(struct vdo_completion *completion)
{
  vdo_start_draining(&zone.state, VDO_ADMIN_STATE_RECOVERING, completion,
                     initiateDrain);
}

/**********************************************************************/
static void testBasic(void)
{
  initializeWithDefaults();
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.clean_pages), 0);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_pages), 0);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);
  CU_ASSERT_EQUAL(cache->page_count, SMALL_CACHE_SIZE);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_reads), 0);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_writes), 0);

  for (struct page_info *info = cache->infos; info < cache->infos + cache->page_count;
       info++) {
    CU_ASSERT_EQUAL(info->busy, 0);
  }

  TestCompletion completions[5];
  for (page_number_t i = 0; i < 5; i++) {
    initializeTestCompletion(&completions[i]);
  }
  TestCompletion pageZeroExtraCompletions[2];
  TestCompletion pageFourExtraCompletions[2];
  for (page_number_t i = 0; i < 2; i++) {
    initializeTestCompletion(&pageZeroExtraCompletions[i]);
    initializeTestCompletion(&pageFourExtraCompletions[i]);
  }

  for (page_number_t i = 0; i < 4; i++) {
    getReadablePage(i, &completions[i]);
  }

  getReadablePage(0, &pageZeroExtraCompletions[0]);

  performPageAction(&completions[0], vdo_release_page_completion);

  launchPageGet(4, false, &completions[4], NULL);
  getReadablePage(0, &pageZeroExtraCompletions[1]);
  launchPageGet(4, false, &pageFourExtraCompletions[0], NULL);
  launchPageGet(4, false, &pageFourExtraCompletions[1], NULL);

  performPageAction(&pageZeroExtraCompletions[0], vdo_release_page_completion);
  performPageAction(&pageZeroExtraCompletions[1], vdo_release_page_completion);

  awaitSuccessfulCompletion(&completions[4]);
  awaitSuccessfulCompletion(&pageFourExtraCompletions[0]);
  awaitSuccessfulCompletion(&pageFourExtraCompletions[1]);

  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);

  for (page_number_t i = 1; i < 5; i++) {
    performPageAction(&completions[i], vdo_release_page_completion);
  }
  performPageAction(&pageFourExtraCompletions[0], vdo_release_page_completion);
  performPageAction(&pageFourExtraCompletions[1], vdo_release_page_completion);

  performSuccessfulAction(flushCacheAction);

  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);

  for (page_number_t i = 0; i < 4; i++) {
    getWritablePage(i, &completions[i]);
    if (i != 0) {
      fillPage(&completions[i], i, 2);
    }
  }

  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 3);

  for (page_number_t i = 0; i < 4; i++) {
    performPageAction(&completions[i], vdo_release_page_completion);
  }

  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 3);

  performSuccessfulAction(flushCacheAction);

  for (struct page_info *info = cache->infos;
       info < cache->infos + cache->page_count;
       info++) {
    CU_ASSERT_EQUAL(info->busy, 0);
  }

  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);
}

/**
 * BIOSumbitHook which fails metadata writes to block 0.
 *
 * Implements BIOSubmitHook.
 **/
static bool failMetaWritesHook(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((bio_op(bio) != REQ_OP_WRITE) || (pbnFromVIO(vio) != 0)) {
    return true;
  }

  bio->bi_status = WRITE_ERROR;
  bio->bi_end_io(bio);
  return false;
}

/**
 * Action to advance the dirty period.
 *
 * <p>Implements vdo_action.
 **/
static void advanceDirtyPeriodAction(struct vdo_completion *completion)
{
  vdo_advance_page_cache_period(cache, period);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * An action to suspend the page cache.
 *
 * <p>Implements vdo_action
 **/
static void suspendCacheAction(struct vdo_completion *completion)
{
  CU_ASSERT(vdo_start_draining(&zone.state, VDO_ADMIN_STATE_SUSPENDING,
                               completion, initiateDrain));
}

/**
 * An action to resume the page cache.
 *
 * <p>Implements vdo_action
 **/
static void resumeCacheAction(struct vdo_completion *completion)
{
  vdo_resume_if_quiescent(&zone.state);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Wait for all page cache I/O to complete.
 **/
static void syncCache(void)
{
  performSuccessfulAction(suspendCacheAction);
  performSuccessfulAction(resumeCacheAction);
}

/**
 * Advance the dirty period.
 *
 * @param newPeriod      The new dirty period
 * @param waitForWrites  If true, wait for any outstanding cache writes
 **/
static void advanceDirtyPeriod(sequence_number_t newPeriod, bool waitForWrites)
{
  period = newPeriod;
  performSuccessfulAction(advanceDirtyPeriodAction);
  if (waitForWrites) {
    syncCache();
  }
}

/**********************************************************************/
static void readOnlyModeListener(void *listener __attribute__((unused)),
                                 struct vdo_completion *parent)
{
  readOnly = true;
  signalState(&readOnly);
  vdo_complete_completion(parent);
}

/**********************************************************************/
static void notEnteringAction(struct vdo_completion *completion)
{
  vdo_wait_until_not_entering_read_only_mode(zone.read_only_notifier,
                                             completion);
}

/**********************************************************************/
static void testReadOnly(void)
{
  initializeWithDefaults();
  TestCompletion completions[3];
  for (page_number_t i = 0; i < 3; i++) {
    initializeTestCompletion(&completions[i]);
  }

  // Dirty page 0.
  getWritablePage(0, &completions[0]);
  readOnly = false;
  setBIOSubmitHook(failMetaWritesHook);
  vdo_register_read_only_listener(zone.read_only_notifier, NULL,
                                  readOnlyModeListener, 0);
  fillPage(&completions[0], 2, 1);
  performPageAction(&completions[0], vdo_release_page_completion);

  // Get page 1.
  getWritablePage(1, &completions[1]);

  // Fail the write of page 0.
  advanceDirtyPeriod(2, false);
  waitForState(&readOnly);
  performSuccessfulAction(notEnteringAction);

  // Dirty page 1.
  fillPage(&completions[1], 3, 2);
  performPageAction(&completions[1], vdo_release_page_completion);

  // Verify reading pages still works, but writing does not.
  getReadablePage(0, &completions[0]);
  getReadablePage(1, &completions[1]);
  launchPageGet(2, true, &completions[2], NULL);
  CU_ASSERT_EQUAL(VDO_READ_ONLY, awaitCompletion(&completions[2].completion));

  performPageAction(&completions[0], vdo_release_page_completion);
  performPageAction(&completions[1], vdo_release_page_completion);
  performPageAction(&completions[2], vdo_release_page_completion);
  performActionExpectResult(suspendCacheAction, VDO_READ_ONLY);
  performSuccessfulAction(resumeCacheAction);

  // Page 0 failed to write, and 1 was dirtied in read-only mode.
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 2);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_reads), 0);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_writes), 1);

  // Flushing should have no effect in read-only mode.
  performActionExpectResult(flushCacheAction, VDO_READ_ONLY);

  // No pages should be busy.
  for (struct page_info *info = cache->infos;
       info < cache->infos + cache->page_count;
       info++) {
    CU_ASSERT_EQUAL(info->busy, 0);
  }


  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 2);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_reads), 0);
  // Page 0 failed to write twice.
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.failed_writes), 2);
}

/**
 * Get a page and perform an action on it. The action must either store or
 * release the page.
 *
 * @param pageNumber  The page to get
 * @param writeable   Whether the page should be writable
 * @param action      The action to perform on the page
 **/
static void withPage(page_number_t pageNumber,
                     bool writable,
                     vdo_action *action)
{
  TestCompletion testCompletion;
  initializeTestCompletion(&testCompletion);
  launchPageGet(pageNumber, writable, &testCompletion, action);
  awaitSuccessfulCompletion(&testCompletion);
}

/**
 * An action to release a page completion.
 *
 * @param completion  The test completion whose page completion should be
 *                    released
 **/
static void releasePageCompletion(struct vdo_completion *completion)
{
  TestCompletion *testCompletion = asTestCompletion(completion);
  vdo_release_page_completion(&testCompletion->pageCompletion.completion);
}

/**
 * Load a page into the cache, but don't do anything to it.
 *
 * @param pageNumber  The page to access
 **/
static void accessPage(page_number_t pageNumber)
{
  withPage(pageNumber, false, releasePageCompletion);
}

/**
 * Action to mark a page dirty.
 *
 * @param completion  The test completion containing the page to mark
 **/
static void markDirty(struct vdo_completion *completion)
{
  markPageDirty(&(asTestCompletion(completion)->pageCompletion.completion));
  releasePageCompletion(completion);
}

/**
 * Load a page into the cache and dirty it.
 *
 * @param pageNumber   The page to dirty
 * @param dirtyPeriod  The period in which this page is to be dirtied
 **/
static void touchPage(page_number_t pageNumber, sequence_number_t dirtyPeriod)
{
  TestCompletion testCompletion;
  initializeTestCompletion(&testCompletion);
  testCompletion.dirtyPeriod = dirtyPeriod;
  launchPageGet(pageNumber, true, &testCompletion, markDirty);
  awaitSuccessfulCompletion(&testCompletion);
}

/**
 * Implements BlockCondition.
 **/
static bool
shouldBlock(struct vdo_completion *completion,
            void                  *context __attribute__((unused)))
{
  return isMetadataWrite(completion);
}

/**
 * An action to check that a page, the last one accessed in the cache,
 * is in the expected state.
 *
 * <p>Implements vdo_action
 **/
static void checkPageAction(struct vdo_completion *completion)
{
  struct page_info *info = int_map_get(cache->page_map, pageCheck.pbn);
  CU_ASSERT_PTR_NOT_NULL(info);
  CU_ASSERT_PTR_EQUAL(info, cache->last_found);
  CU_ASSERT_EQUAL(info->pbn, pageCheck.pbn);
  CU_ASSERT_EQUAL(info->busy, pageCheck.busyCount);
  CU_ASSERT_EQUAL(info->state, pageCheck.state);
  CU_ASSERT_EQUAL(info->write_status, pageCheck.writeStatus);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Check the properties of a page which should be in the cache, and should
 * be the last found page in the cache.
 *
 * @param pbn          The pbn of the page
 * @param busyCount    The expected busy count of the page
 * @param state        The expected buffer state of the page
 * @param writeStatus  The expected write status of the page
 **/
static void checkPage(physical_block_number_t    pbn,
                      uint16_t                   busyCount,
                      enum vdo_page_buffer_state state,
                      enum vdo_page_write_status writeStatus)
{
  pageCheck = (PageCheck) {
    .pbn         = pbn,
    .busyCount   = busyCount,
    .state       = state,
    .writeStatus = writeStatus,
  };
  performSuccessfulAction(checkPageAction);
}

/**
 * Test that attempting to get a busy page while it is locked does not result
 * in utter failure.
 **/
static void testBusyCachePage(void)
{
  initialize(SMALL_CACHE_SIZE, 1);

  // Make some pages dirty.
  touchPage(0, 1);
  touchPage(1, 1);
  touchPage(2, 1);
  touchPage(3, 1);

  // Get a page 1 completion and hold on to it.
  TestCompletion p1comp;
  initializeTestCompletion(&p1comp);
  getReadablePage(1, &p1comp);

  // Verify the page is busy.
  checkPage(1, 1, PS_DIRTY, WRITE_STATUS_NORMAL);

  advanceDirtyPeriod(2, true);

  // Verify the page is still busy and deferred.
  checkPage(1, 1, PS_DIRTY, WRITE_STATUS_DEFERRED);

  // Get another reference to the page, this should block.
  TestCompletion p1again;
  initializeTestCompletion(&p1again);

  getRequested = false;
  launchPageGet(1, false, &p1again, NULL);
  waitForState(&getRequested);

  // Block the next metadata write
  setBlockBIO(shouldBlock, true);

  // Release the original reference, should trigger saving.
  performPageAction(&p1comp, vdo_release_page_completion);

  // The page is no longer busy.
  checkPage(1, 0, PS_OUTGOING, WRITE_STATUS_NORMAL);

  // Wait for items to be trapped and resubmit them.
  reallyEnqueueBIO(getBlockedVIO()->bio);

  // Wait for second reference to complete.
  awaitSuccessfulCompletion(&p1again);

  // The page should no longer be deferred or dirty, but is busy again.
  checkPage(1, 1, PS_RESIDENT, WRITE_STATUS_NORMAL);
  performPageAction(&p1again, vdo_release_page_completion);

  // The page shouldn't be busy either.
  checkPage(1, 0, PS_RESIDENT, WRITE_STATUS_NORMAL);

  // The cache should be clean
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);
}

/**
 * Action to get a page and dereference it for reading.
 *
 * <p>Implements vdo_action
 **/
static void accessReadablePage(struct vdo_completion *completion)
{
  const TestPage *page = vdo_dereference_readable_page(completion);
  CU_ASSERT_PTR_NOT_NULL(page);
  CacheEntryUID *uidContext = vdo_get_page_completion_context(completion);
  CU_ASSERT_PTR_NOT_NULL(uidContext);
  CU_ASSERT_EQUAL(*uidContext, page->header.uid);
}

/**
 * Action to get a page and dereference it for writing.
 *
 * <p>Implements vdo_action
 **/
static void accessWritablePage(struct vdo_completion *completion)
{
  TestPage *page = vdo_dereference_writable_page(completion);
  CU_ASSERT_PTR_NOT_NULL(page);
  CacheEntryUID *uidContext = vdo_get_page_completion_context(completion);
  CU_ASSERT_PTR_NOT_NULL(uidContext);
  CU_ASSERT_EQUAL(*uidContext, page->header.uid);
}

/**
 * Action to confirm that getting a page and dereferencing it for writing
 * fails to return a page.
 *
 * <p>Implements vdo_action
 **/
static void failAccessingWritablePage(struct vdo_completion *completion)
{
  char *data = vdo_dereference_writable_page(completion);
  CU_ASSERT_PTR_NULL(data);
}

/**********************************************************************/
static void testAccessMode(void)
{
  initializeWithDefaults();

  TestCompletion readOnly;
  TestCompletion writable;
  initializeTestCompletion(&readOnly);
  initializeTestCompletion(&writable);

  getReadablePage(1, &readOnly);
  getWritablePage(2, &writable);

  performPageAction(&readOnly, accessReadablePage);
  performPageAction(&writable, accessReadablePage);

  performPageAction(&writable, accessWritablePage);

  set_exit_on_assertion_failure(false);
  performPageAction(&readOnly, failAccessingWritablePage);
  set_exit_on_assertion_failure(true);

  performPageAction(&readOnly, vdo_release_page_completion);
  performPageAction(&writable, vdo_release_page_completion);
}

/**********************************************************************/
static void touchPages(page_number_t     start,
                       page_number_t     end,
                       sequence_number_t period)
{
  for (page_number_t i = start; i < end; i++) {
    touchPage(i, period);
  }
}

/**
 * Increment the dirty period asserting that the number of dirty pages before
 * and after are as expected.
 *
 * @param dirtyBefore  The expected number of dirty pages before advancing
 * @param dirtyAfter   The expected number of dirty page after advancing
 **/
static void advanceAndAssert(uint64_t dirtyBefore, uint64_t dirtyAfter)
{
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), dirtyBefore);
  advanceDirtyPeriod(period + 1, true);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), dirtyAfter);
}

/**********************************************************************/
static void testAgeDirtyPages(void)
{
  initialize(LARGE_CACHE_SIZE, 2);
  for (page_number_t i = 0; i < LARGE_CACHE_SIZE; i++) {
    accessPage(i);
  }
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);

  // Dirty pages 0-2 in period 1.
  touchPages(0, 3, 1);
  // Advance the current period to 2, nothing should get written out.
  advanceAndAssert(3, 3);
  // Dirty pages 0-3 in period 2.
  touchPages(0, 4, period);
  // Advance the current period to 3, pages 0-2 should get written out.
  advanceAndAssert(4, 1);
  // Dirty pages 0-3 in period 3.
  touchPages(0, 4, period);
  // Advance the current period to 4, page 3 should get writtn out.
  advanceAndAssert(4, 3);
  // Dirty pages 0 and 1 twice in period 4.
  touchPages(0, 2, period);
  touchPages(0, 2, period);
  // Advance the current period to 5, pages 0-3 should get written out.
  advanceAndAssert(3, 0);
  // Dirty page 0 in period 3, it should get written out immediately.
  touchPage(0, period - 2);
  syncCache();
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);
  // Dirty page 1 in period 4
  touchPage(1, period - 1);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 1);
  // Dirty page 2 in period 5
  touchPage(2, period);
  // Advance to period 6, page 1 should get written out
  advanceAndAssert(2, 1);
  // Advance to period 7, page 0 should get written out
  advanceAndAssert(1, 0);
  // Advance to period 8
  advanceAndAssert(0, 0);
  // Dirty pages 0 and 1 in period 7
  touchPages(0, 2, period - 1);
  syncCache();
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 2);
  // Dirty pages 2 and 3 in period 8
  touchPages(2, 4, period);
  syncCache();
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 4);
  // Dirty pages 4 and 5 in period 9, pages 0 and 1 should get written out
  touchPages(4, 6, period + 1);
  syncCache();
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 4);
  // Advance to period 12, everything should get written out
  advanceDirtyPeriod(12, true);
  CU_ASSERT_EQUAL(READ_ONCE(cache->stats.dirty_pages), 0);

  // Nothing should be busy
  for (struct page_info *info = cache->infos;
       info < cache->infos + cache->page_count;
       info++) {
    CU_ASSERT_EQUAL(info->busy, 0);
  }
}

/**********************************************************************/

static CU_TestInfo vdoPageCacheTests[] = {
  { "basic functionality", testBasic         },
  { "read-only",           testReadOnly      },
  { "busy cache page",     testBusyCachePage },
  { "access mode",         testAccessMode    },
  { "age dirty eras",      testAgeDirtyPages },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoPageCacheSuite = {
  .name                     = "VDO Page Cache tests (VDOPageCache_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = finishVDOPageCacheT1,
  .tests                    = vdoPageCacheTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoPageCacheSuite;
}
