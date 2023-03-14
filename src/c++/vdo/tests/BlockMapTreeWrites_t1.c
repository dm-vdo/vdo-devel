/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "action-manager.h"
#include "admin-state.h"
#include "block-map.h"
#include "encodings.h"
#include "recovery-journal.h"
#include "vio.h"

#include "asyncLayer.h"
#include "asyncVIO.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  ENTRIES_PER_BLOCK         = 16,
  WRITES_PER_BLOCK          = ENTRIES_PER_BLOCK,
  INTERIOR_HEIGHT           = VDO_BLOCK_MAP_TREE_HEIGHT - 1,
  NEW_TREE_WRITES_PER_BLOCK = (ENTRIES_PER_BLOCK - INTERIOR_HEIGHT),
};

static struct vio              *blockedWriter;
static block_count_t            writeCount;
static struct block_map_zone   *zone;
static bool                     fourWaiters;
static bool                     notOperating;
static bool                     writeBlocked;
static physical_block_number_t  pbn;
static uint8_t                  flushGeneration;
static uint8_t                  writeGeneration;

/**
 * Test-specific initialization.
 *
 * @param journalBlocks  The number of recovery journal blocks
 **/
static void initialize(block_count_t journalBlocks)
{
  TestParameters parameters = {
    .mappableBlocks = 1024,
    .logicalBlocks  = (DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 4
                       * VDO_BLOCK_MAP_ENTRIES_PER_PAGE),
    .journalBlocks  = journalBlocks,
    .dataFormatter  = fillWithOffsetPlusOne,
  };

  flushGeneration = 0xFF;
  writeCount      = 0;
  writeGeneration = 0;
  initializeVDOTest(&parameters);

  vdo->recovery_journal->entries_per_block = ENTRIES_PER_BLOCK;
  zone = &vdo->block_map->zones[0];
}

/**
 * Convert a pooled VIO pointer to the tree_page that owns it.
 *
 * @param vio  The pointer to convert
 *
 * @return The tree_page using the pooled VIO to write itself
 **/
__attribute__((warn_unused_result))
static inline struct tree_page *findParentTreePage(struct vio *vio)
{
  if (vio == NULL) {
    return NULL;
  }

  return (struct tree_page *) vio->completion.parent;
}

/**
 * Implements LockedMethod.
 **/
static bool incrementWriteCount(void *context __attribute__((unused)))
{
  writeCount++;
  return true;
}

/**
 * Check whether a completion is a vio which has just done an initialized write
 * of an interior tree page.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is doing such a write
 **/
static bool isInitializedInteriorPageWrite(struct vdo_completion *completion)
{
  if (!onBIOThread() || !vioTypeIs(completion, VIO_TYPE_BLOCK_MAP_INTERIOR)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  struct block_map_page *page = (struct block_map_page *) vio->data;
  return ((bio_op(vio->bio) == REQ_OP_WRITE) && page->header.initialized);
}

/**
 * Check the generation of a VIO doing an initialized write of an interior tree
 * page.
 *
 * @param vio  The VIO to check
 *
 * @return <code>true</code> if this VIO is the flusher
 **/
static bool assertGeneration(struct vio *vio)
{
  struct tree_page *treePage = findParentTreePage(vio);
  if (isPreFlush(vio)) {
    CU_ASSERT_EQUAL(treePage->writing_generation, (uint8_t) (flushGeneration + 1));
    flushGeneration = treePage->writing_generation;
    return true;
  }

  CU_ASSERT_EQUAL(treePage->writing_generation, flushGeneration);
  return false;
}

/**
 * Implements VDOAction.
 **/
static void countWrite(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  runLocked(incrementWriteCount, NULL);
}

/**
 * Implements CompletionHook.
 **/
static bool countWriteHook(struct vdo_completion *completion)
{
  if (!isInitializedInteriorPageWrite(completion)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  assertGeneration(vio);
  wrapVIOCallback(vio, countWrite);
  return true;
}

/**
 * Implements BlockCondition.
 **/
static bool
blockFlusherSecondWrite(struct vdo_completion *completion,
                        void                  *context __attribute__((unused)))
{
  if (!isInitializedInteriorPageWrite(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  bool block = assertGeneration(vio);
  if (block && !writeBlocked) {
    writeBlocked = true;
  }

  wrapVIOCallback(vio, countWrite);
  return block;
}

/**
 * Implements WaitCondition.
 **/
static bool checkWriteCount(void *context)
{
  return (writeCount >= *((block_count_t *) context));
}

/**
 * Wait for the write count to meet or exceed a stated amount.
 *
 * @param target The target write count
 **/
static void waitForWrites(block_count_t target)
{
  waitForCondition(checkWriteCount, &target);
}

/**********************************************************************/
static struct vio *advanceJournalUntilFlusherBlocked(void)
{
  while (!checkState(&writeBlocked)) {
    writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  }

  struct vio *flusher = getBlockedVIO();
  clearState(&writeBlocked);
  return flusher;
}

/**********************************************************************/
static void recordFirstWaiterPBN(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(vdo_count_waiters(&zone->flush_waiters), 3);
  struct tree_page *treePage
    = container_of(vdo_get_first_waiter(&zone->flush_waiters),
                   struct tree_page, waiter);
  pbn = vdo_get_block_map_page_pbn(vdo_as_block_map_page(treePage));
  vdo_finish_completion(completion);
}

/**
 * Verify that tree pages are properly flushed in async mode.
 **/
static void testBlockMapTreeWrites(void)
{
  block_count_t journalLength = 8;
  initialize(journalLength);

  // Make dirty pages up to the root in the first four trees. Then advance the
  // journal until all of the pages are written out.
  root_count_t trees = 4;
  setCompletionEnqueueHook(countWriteHook);
  for (root_count_t i = 0; i < trees; i++) {
    writeData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 0, (ENTRIES_PER_BLOCK - 4), VDO_SUCCESS);
  }

  block_count_t writeTarget = trees * INTERIOR_HEIGHT;
  while (!checkCondition(checkWriteCount, &writeTarget)) {
    writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  }

  /*
   * Redirty the bottom node from each of the four trees, and advance the
   * journal so that the dirty pages are written, but block the flush.
   */
  clearState(&writeBlocked);
  setBlockVIOCompletionEnqueueHook(blockFlusherSecondWrite, false);
  writeTarget += trees;
  for (root_count_t i = 0; i < trees; i++) {
    writeData(((i + DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT)
               * VDO_BLOCK_MAP_ENTRIES_PER_PAGE), 0, 1, VDO_SUCCESS);
  }

  struct vio *flusher = advanceJournalUntilFlusherBlocked();
  pbn = vdo_get_block_map_page_pbn((struct block_map_page *) flusher->data);
  // Redirty the flusher, but since it is already out for writing, it will just
  // go back on the dirty list.
  writeData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE
              * DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 2,
            0, 1, VDO_SUCCESS);
  reallyEnqueueVIO(flusher);
  waitForWrites(writeTarget);

  // Dirty the bottom nodes of the other trees again and advance until they are
  // expired. Once again block the flusher.
  writeTarget += trees;
  for (root_count_t i = 1; i < trees; i++) {
    writeData(((i + (DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 2))
               * VDO_BLOCK_MAP_ENTRIES_PER_PAGE), 0, 1, VDO_SUCCESS);
  }

  flusher = advanceJournalUntilFlusherBlocked();
  struct block_map_page *blockMapPage
    = (struct block_map_page *) flusher->data;
  CU_ASSERT_EQUAL(pbn, vdo_get_block_map_page_pbn(blockMapPage));

  // Redirty all the non-flusher bottom nodes again while they are waiting.
  CU_ASSERT_FALSE(checkCondition(checkWriteCount, &writeTarget));
  for (root_count_t i = 1; i < trees; i++) {
    writeData(((i + (DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 3))
               * VDO_BLOCK_MAP_ENTRIES_PER_PAGE), 0, 1, VDO_SUCCESS);
  }

  // Upon release of the flusher, the next waiter should immediately become
  // the next flusher.
  performSuccessfulActionOnThread(recordFirstWaiterPBN, zone->thread_id);
  reallyEnqueueVIO(flusher);
  flusher = getBlockedVIO();
  blockMapPage = (struct block_map_page *) flusher->data;
  CU_ASSERT_EQUAL(pbn, vdo_get_block_map_page_pbn(blockMapPage));

  // Upon releaes of the flusher, everything should write out
  reallyEnqueueVIO(flusher);
  CU_ASSERT_FALSE(checkCondition(checkWriteCount, &writeTarget));
  waitForWrites(writeTarget);

  // Lap the entire journal and check that no more writes occurred (i.e. that
  // everything was already written out.
  for (block_count_t i = 0; i < journalLength; i++) {
    writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  }
  writeTarget++;
  CU_ASSERT_FALSE(checkCondition(checkWriteCount, &writeTarget));
}

/**
 * Implements BlockCondition.
 **/
static bool
blockFirstNotFlusherCountAllWrites(struct vdo_completion *completion,
                                   void *context __attribute__((unused)))
{
  if (!isInitializedInteriorPageWrite(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  wrapVIOCallback(vio, countWrite);
  if (writeBlocked || isPreFlush(vio)) {
    return false;
  }

  writeBlocked = true;
  return true;
}

/**
 * Implements VDOAction.
 **/
static void skipGenerations(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(zone->generation, 1);
  zone->generation = 254;
  vdo_finish_completion(completion);
}

/**
 * Implements FinishedHook.
 **/
static void countTreeWaiters(void)
{
  if (vdo_get_callback_thread_id() == zone->thread_id) {
    size_t waiters = vdo_count_waiters(&zone->flush_waiters);
    CU_ASSERT_TRUE(waiters <= 4);
    if (waiters == 4) {
      signalState(&fourWaiters);
    }
  }
}

/**
 * Implements CompletionHook.
 **/
static bool countFinalWriters(struct vdo_completion *completion)
{
  if (!isInitializedInteriorPageWrite(completion)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  if (vio == blockedWriter) {
    blockedWriter = NULL;
    return true;
  }

  struct tree_page *treePage = findParentTreePage(vio);
  CU_ASSERT_EQUAL(treePage->writing_generation, 255);
  wrapVIOCallback(vio, countWrite);
  return true;
}

static void checkNotOperating(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  if (vdo_get_current_manager_operation(vdo->block_map->action_manager)
      == VDO_ADMIN_STATE_NORMAL_OPERATION) {
    signalState(&notOperating);
  }
}

static bool wrapEraAdvance(struct vdo_completion *completion)
{
  if (completion->type != VDO_ACTION_COMPLETION) {
    return true;
  }

  if (completion->callback_thread_id == vdo->thread_config->admin_thread) {
    wrapCompletionCallback(completion, checkNotOperating);
  }

  return true;
}

/**
 * Verify that tree pages are properly flushed in async mode.
 **/
static void testBlockMapTreeGenerationRollOver(void)
{
  initialize(16);

  // Make 2 fewer than the era length dirty generations.
  clearState(&writeBlocked);
  setBlockVIOCompletionEnqueueHook(blockFirstNotFlusherCountAllWrites, false);
  u32 eras = vdo_convert_maximum_age(vdo->device_config->block_map_maximum_age) - 2;
  for (block_count_t i = 0; i < eras; i++) {
    writeData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE * i, 0, NEW_TREE_WRITES_PER_BLOCK,
              VDO_SUCCESS);
  }

  /*
   * Advance the journal by two blocks so that the first batch of dirty pages
   * is written. Block one of the non-flushers. But do the writes incrementally so
   * that we know the era will be advanced.
   */
  notOperating = false;
  addCompletionEnqueueHook(wrapEraAdvance);
  writeData(0, 0, 1, VDO_SUCCESS);
  waitForState(&notOperating);
  removeCompletionEnqueueHook(wrapEraAdvance);
  writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  blockedWriter = getBlockedVIO();

  block_count_t writeTarget = INTERIOR_HEIGHT - 1;
  waitForWrites(writeTarget);

  // Skip generations so that the next batch will wrap the counter
  performSuccessfulActionOnThread(skipGenerations, zone->thread_id);

  // Advance the journal one more block which should write one more batch.
  writeTarget += INTERIOR_HEIGHT;
  writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  waitForWrites(writeTarget);

  // Advance the journal one more block and confirm that the last batch of
  // pages are waiting
  writeTarget += INTERIOR_HEIGHT;
  clearState(&fourWaiters);
  setCallbackFinishedHook(countTreeWaiters);
  writeData(0, 0, WRITES_PER_BLOCK, VDO_SUCCESS);
  waitForState(&fourWaiters);

  // Release the blocked writer and confirm that all dirty pages get written.
  setCompletionEnqueueHook(countFinalWriters);
  reallyEnqueueVIO(blockedWriter);
  waitForWrites(writeTarget);
}

/**
 * Implements BlockCondition.
 **/
static bool
blockNotFlusher(struct vdo_completion *completion,
                void                  *context __attribute__((unused)))
{
  if (!isInitializedInteriorPageWrite(completion)) {
    return false;
  }

  if (isPreFlush(as_vio(completion))) {
    return false;
  }

  writeBlocked = true;
  return true;
}

/**
 * Implements CompletionHook.
 **/
static bool checkFinalWrites(struct vdo_completion *completion)
{
  if (!onBIOThread()
      || !isMetadataWrite(completion)
      || !vioTypeIs(completion, VIO_TYPE_BLOCK_MAP_INTERIOR)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  if (isPreFlush(vio)) {
    writeGeneration++;
  }

  if (((struct block_map_page *) vio->data)->header.initialized) {
    struct tree_page *treePage = findParentTreePage(vio);
    CU_ASSERT_EQUAL(treePage->writing_generation, writeGeneration);
    wrapVIOCallback(vio, countWrite);
  }

  return true;
}

/**
 * Verify that tree pages are properly redirtied when the VIO pool is
 * exhausted.
 **/
static void testBlockMapTreeWritesWithExhaustedVIOPool(void)
{
  initialize(8);

  /* Replace the zone's vio pool with one which only has 1 vio */
  free_vio_pool(UDS_FORGET(zone->vio_pool));
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo,
                                   1,
                                   zone->thread_id,
                                   VIO_TYPE_BLOCK_MAP_INTERIOR,
                                   VIO_PRIORITY_METADATA,
                                   zone,
                                   &zone->vio_pool));

  /*
   * Make some dirty tree pages and advance one journal block so that the
   * dirty block map tree pages are written, but block the write of the first
   * non-flusher.
   */
  writeBlocked = false;
  setBlockVIOCompletionEnqueueHook(blockNotFlusher, true);
  addCompletionEnqueueHook(wrapEraAdvance);
  for (u8 i = 0; i < 3; i++) {
    notOperating = false;
    writeData(0, 1, (i == 0) ? NEW_TREE_WRITES_PER_BLOCK : ENTRIES_PER_BLOCK, VDO_SUCCESS);
    waitForState(&notOperating);
  }
  removeCompletionEnqueueHook(wrapEraAdvance);
  struct vio *writer = getBlockedVIO();

  // Redirty one of the two waiting dirty pages
  setCompletionEnqueueHook(checkFinalWrites);
  writeData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE * DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
            1,
            ENTRIES_PER_BLOCK,
            VDO_SUCCESS);
  writeCount = 0;
  reallyEnqueueVIO(writer);
  waitForWrites(2);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test block map tree writing and flushing", testBlockMapTreeWrites },
  { "test block map tree generation wrap",
    testBlockMapTreeGenerationRollOver },
  { "test block map tree writing with exhausted VIOPool",
    testBlockMapTreeWritesWithExhaustedVIOPool },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "check block map tree writing and flushing (BlockMapTreeWrites_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
