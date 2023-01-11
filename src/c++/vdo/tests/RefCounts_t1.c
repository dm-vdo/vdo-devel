/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "block-allocator.h"
#include "read-only-notifier.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "status-codes.h"
#include "vdo.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "latchedCloseUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // Ensure multiple reference count blocks.
  SLAB_SIZE          = VDO_BLOCK_SIZE * 2,
  JOURNAL_SIZE       = 2,
  FIRST_BLOCK        = 1,
  TEST_VIO_POOL_SIZE = 2,
};

static struct ref_counts         *refs;
static struct ref_counts         *loaded;
static struct fixed_layout       *layout;
static struct slab_depot         *depot;
static struct block_allocator     allocator;
static struct read_only_notifier *readOnlyNotifier;
static struct thread_config      *threadConfig;
static struct vdo_slab           *slab;
static physical_block_number_t    pbnToBlock;
static block_count_t              viosFinishedCount;
static block_count_t              desiredFinishedCount;
static bool                       refCountsCompletionWaiting;
static int                        expectedCloseResult;

/**
 * Read-only notification.
 **/
static void readOnlyNotification(void *listener __attribute__((unused)),
                                 struct vdo_completion *parent)
{
  expectedCloseResult = VDO_READ_ONLY;
  vdo_complete_completion(parent);
}

/**********************************************************************/
static void allowEnteringAction(struct vdo_completion *completion)
{
  vdo_allow_read_only_mode_entry(readOnlyNotifier, completion);
}

/**********************************************************************/
static void initializeRefCountsT1(void)
{
  TestParameters testParameters = {
    .slabSize = VDO_BLOCK_SIZE * 2,
    .noIndexRegion = true,
  };
  initializeBasicTest(&testParameters);

  // This test assumes reference blocks are initialized to zero. So
  // clear out RAM layer with zeros.
  zeroRAMLayer(getSynchronousLayer());

  VDO_ASSERT_SUCCESS(UDS_ALLOCATE_EXTENDED(struct slab_depot, 1,
                                           struct block_allocator *,
                                           __func__, &depot));
  depot->allocators[0] = &allocator;
  allocator.depot      = depot;


  threadConfig = makeOneThreadConfig();
  VDO_ASSERT_SUCCESS(vdo_make_read_only_notifier(false,
                                                 threadConfig,
                                                 vdo,
                                                 &readOnlyNotifier));
  performSuccessfulAction(allowEnteringAction);
  expectedCloseResult = VDO_SUCCESS;
  VDO_ASSERT_SUCCESS(vdo_register_read_only_listener(readOnlyNotifier, NULL,
                                                     readOnlyNotification, 0));

  allocator.read_only_notifier = readOnlyNotifier;

  viosFinishedCount          = 0;
  refCountsCompletionWaiting = false;

  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout(layer->getBlockCount(layer), 0,
                                           &layout));

  int result = vdo_make_fixed_layout_partition(layout,
                                               VDO_SLAB_SUMMARY_PARTITION,
                                               VDO_SLAB_SUMMARY_BLOCKS,
                                               VDO_PARTITION_FROM_END,
                                               0);
  VDO_ASSERT_SUCCESS(result);

  struct partition *slabSummaryPartition;
  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout,
						    VDO_SLAB_SUMMARY_PARTITION,
						    &slabSummaryPartition));
  VDO_ASSERT_SUCCESS(vdo_make_slab_summary(vdo,
                                           slabSummaryPartition,
                                           threadConfig,
                                           depot->slab_size_shift,
                                           SLAB_SIZE,
                                           readOnlyNotifier,
                                           &depot->slab_summary));
  allocator.summary = vdo_get_slab_summary_for_zone(depot->slab_summary, 0);

  VDO_ASSERT_SUCCESS(vdo_configure_slab(SLAB_SIZE, JOURNAL_SIZE,
                                        &depot->slab_config));
  VDO_ASSERT_SUCCESS(make_priority_table(63, &allocator.prioritized_slabs));
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo,
                                   TEST_VIO_POOL_SIZE,
                                   allocator.thread_id,
                                   VIO_TYPE_SLAB_JOURNAL,
                                   VIO_PRIORITY_METADATA,
                                   &allocator,
                                   &allocator.vio_pool));
  VDO_ASSERT_SUCCESS(vdo_make_slab(FIRST_BLOCK, &allocator, 1, NULL, 0, false,
                                   &slab));
  VDO_ASSERT_SUCCESS(vdo_allocate_ref_counts_for_slab(slab));

  /*
   * Set the slab to be rebuilding so that slab journal locks will be ignored.
   * Since this test doesn't maintain the correct lock invariants, it would
   * fail on a lock count underflow otherwise.
   */
  slab->status = VDO_SLAB_REPLAYING;
  refs         = slab->reference_counts;
}

/**********************************************************************/
static void notEnteringAction(struct vdo_completion *completion)
{
  vdo_wait_until_not_entering_read_only_mode(readOnlyNotifier, completion);
}

/**********************************************************************/
static void tearDownRefCountsT1(void)
{
  performSuccessfulAction(notEnteringAction);
  CU_ASSERT_EQUAL(expectedCloseResult, closeSlabSummary(depot->slab_summary));
  free_priority_table(UDS_FORGET(allocator.prioritized_slabs));
  vdo_free_slab(UDS_FORGET(slab));
  free_vio_pool(UDS_FORGET(allocator.vio_pool));
  vdo_free_slab_summary(UDS_FORGET(depot->slab_summary));
  vdo_free_read_only_notifier(UDS_FORGET(readOnlyNotifier));
  vdo_free_thread_config(UDS_FORGET(threadConfig));
  UDS_FREE(depot);
  vdo_free_fixed_layout(UDS_FORGET(layout));
  tearDownVDOTest();
}

/**
 * Assert the value of the reference status of a given block number.
 *
 * @param pbn               The physical block number to check
 * @param expectedStatus    The expected reference status
 **/
static void assertReferenceStatus(physical_block_number_t pbn,
                                  enum reference_status   expectedStatus)
{
  enum reference_status status;
  VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &status));
  CU_ASSERT_EQUAL(expectedStatus, status);
}

/**
 * Perform a reference count adjustment and assert the return value.
 *
 * @param pbn                        The physical block number to adjust
 * @param slabJournalPoint           The journal point of the slab journal
 *                                   entry for this adjustment
 * @param operation                  The type of adjustment to perform
 * @param expectedResult             The expected result of the adjustment
 * @param expectedFreeStatusChanged  Whether the free status should change
 **/
static void
performAdjustment(physical_block_number_t     pbn,
                  const struct journal_point *slabJournalPoint,
                  enum journal_operation      operation,
                  int                         expectedResult,
                  bool                        expectedFreeStatusChanged)
{
  bool freeStatusChanged = ((expectedResult == VDO_SUCCESS)
                            ? !expectedFreeStatusChanged
                            : expectedFreeStatusChanged);
  struct reference_operation referenceOperation = {
    .pbn  = pbn,
    .type = operation,
  };
  CU_ASSERT_EQUAL(vdo_adjust_reference_count(refs, referenceOperation,
                                             slabJournalPoint,
                                             &freeStatusChanged),
                  expectedResult);
  CU_ASSERT_EQUAL(expectedFreeStatusChanged, freeStatusChanged);
}

/**
 * Adjust a reference count and check that the resulting status is as expected.
 *
 * @param pbn               The physical block number to adjust
 * @param slabJournalPoint  The journal point of the slab journal entry for
 *                          this adjustment
 * @param increment         Whether the adjustment is an increment or a
 *                          decrement
 * @param expectedStatus    The expected reference status after the adjustment
 **/
static void assertAdjustment(physical_block_number_t     pbn,
                             const struct journal_point *slabJournalPoint,
                             bool                        increment,
                             enum reference_status       expectedStatus)
{
  bool expectedFreeStatusChanged;
  if (expectedStatus == RS_FREE) {
    expectedFreeStatusChanged = !increment;
  } else {
    enum reference_status oldStatus;
    VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &oldStatus));
    expectedFreeStatusChanged = ((oldStatus == RS_FREE) && increment);
  }

  block_count_t freeBefore = vdo_get_unreferenced_block_count(refs);
  performAdjustment(pbn, slabJournalPoint, increment, VDO_SUCCESS,
                    expectedFreeStatusChanged);
  block_count_t freeAfter = vdo_get_unreferenced_block_count(refs);

  assertReferenceStatus(pbn, expectedStatus);

  if (expectedFreeStatusChanged) {
    freeBefore += (increment ? -1 : 1);
  }
  CU_ASSERT_EQUAL(freeAfter, freeBefore);
}

/**********************************************************************/
static void assertAllocation(physical_block_number_t expectedPBN)
{
  physical_block_number_t allocatedPBN;
  VDO_ASSERT_SUCCESS(vdo_allocate_unreferenced_block(refs, &allocatedPBN));
  CU_ASSERT_EQUAL(expectedPBN, allocatedPBN);
}

/**********************************************************************/
static void assertFailedAdjustment(physical_block_number_t pbn,
                                   bool                    increment,
                                   int                     expectedResult)
{
  enum reference_status oldStatus;
  VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &oldStatus));
  performAdjustment(pbn, NULL, increment, expectedResult, false);
  assertReferenceStatus(pbn, oldStatus);
}

/**********************************************************************/
static void assertFailedDecrement(physical_block_number_t pbn)
{
  assertFailedAdjustment(pbn, false, VDO_REF_COUNT_INVALID);
}

/**********************************************************************/
static void addManyReferences(physical_block_number_t pbn, uint8_t howMany)
{
  for (uint8_t i = 0; i < howMany; i++) {
    bool freeStatusChanged;
    struct reference_operation operation = {
      .pbn  = pbn,
      .type = VDO_JOURNAL_DATA_INCREMENT,
    };
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &freeStatusChanged));
  }
}

/**********************************************************************/
static void assertBlockMapIncrement(physical_block_number_t pbn)
{
  block_count_t freeBefore = vdo_get_unreferenced_block_count(refs);
  performAdjustment(pbn, NULL, VDO_JOURNAL_BLOCK_MAP_INCREMENT, VDO_SUCCESS,
                    false);
  assertReferenceStatus(pbn, RS_SHARED);
  // The block was already counted as not free when it was provisionally
  // referenced.
  CU_ASSERT_EQUAL(freeBefore,
                  vdo_get_unreferenced_block_count(refs));
  assertFailedAdjustment(pbn, true, VDO_REF_COUNT_INVALID);
}

/**
 * Most basic refCounts test.
 **/
static void testBasic(void)
{
  enum reference_status refStatus;
  block_count_t         dataBlocks = depot->slab_config.data_blocks;
  for (physical_block_number_t pbn = FIRST_BLOCK; pbn < dataBlocks; pbn++) {
    assertReferenceStatus(pbn, RS_FREE);
  }
  CU_ASSERT_EQUAL(dataBlocks,
                  vdo_get_unreferenced_block_count(refs));

  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE,
                  vdo_get_reference_status(refs, FIRST_BLOCK - 1, &refStatus));
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE,
                  vdo_get_reference_status(refs, FIRST_BLOCK + dataBlocks,
                                           &refStatus));

  assertAdjustment(1, NULL,  true, RS_SINGLE);
  assertAdjustment(1, NULL,  true, RS_SHARED);
  assertAdjustment(2, NULL,  true, RS_SINGLE);
  assertAdjustment(2, NULL,  true, RS_SHARED);
  assertAdjustment(2, NULL, false, RS_SINGLE);
  assertAdjustment(2, NULL, false, RS_FREE);
  assertAdjustment(1, NULL,  true, RS_SHARED);
  assertAdjustment(1, NULL, false, RS_SHARED);
  assertAdjustment(1, NULL, false, RS_SINGLE);
  assertAdjustment(1, NULL, false, RS_FREE);

  assertFailedDecrement(1);

  assertAllocation(1);
  CU_ASSERT_EQUAL(dataBlocks - 1,
                  vdo_get_unreferenced_block_count(refs));
  assertReferenceStatus(1, RS_PROVISIONAL);

  assertAdjustment(3, NULL, true, RS_SINGLE);
  CU_ASSERT_EQUAL(dataBlocks - 2,
                  vdo_get_unreferenced_block_count(refs));

  assertAllocation(2);
  CU_ASSERT_EQUAL(dataBlocks - 3,
                  vdo_get_unreferenced_block_count(refs));
  assertReferenceStatus(2, RS_PROVISIONAL);

  // Block #3 was manually incRef'ed, so it will be skipped and #4 allocated.
  assertAllocation(4);
  CU_ASSERT_EQUAL(dataBlocks - 4,
                  vdo_get_unreferenced_block_count(refs));
  assertReferenceStatus(4, RS_PROVISIONAL);
  assertAdjustment(4, NULL, false, RS_FREE);
  assertFailedDecrement(4);

  addManyReferences(5, 254);
  assertReferenceStatus(5, RS_SHARED);

  assertFailedDecrement(6);

  // Test block map increment succeeds for a provisionally referenced block.
  assertBlockMapIncrement(1);

  // Test block map increments fail for RS_FREE.
  performAdjustment(4, NULL, VDO_JOURNAL_BLOCK_MAP_INCREMENT,
                    VDO_REF_COUNT_INVALID, false);
  // Test block map increments fail for RS_SINGLE.
  performAdjustment(3, NULL, VDO_JOURNAL_BLOCK_MAP_INCREMENT,
                    VDO_REF_COUNT_INVALID, false);
  // Test block map increments fail for RS_SHARED.
  assertAdjustment(3, NULL, true, RS_SHARED);
  performAdjustment(3, NULL, VDO_JOURNAL_BLOCK_MAP_INCREMENT,
                    VDO_REF_COUNT_INVALID, false);
}

/**
 * Action wrapper to modify first refcount on first block.
 **/
static void dirtyFirstBlockAction(struct vdo_completion *completion)
{
  addManyReferences(FIRST_BLOCK, 1);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Action wrapper to modify second refcount on first block.
 **/
static void redirtyFirstBlockAction(struct vdo_completion *completion)
{
  addManyReferences(FIRST_BLOCK + 1, 1);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Action wrapper to modify a refcount on the second block.
 **/
static void dirtySecondBlockAction(struct vdo_completion *completion)
{
  addManyReferences(FIRST_BLOCK + VDO_BLOCK_SIZE, 1);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Action wrapper to fire off all dirty blocks.
 **/
static void saveDirtyBlocksAction(struct vdo_completion *completion)
{
  // Fire off every dirty reference block in the queue at once.
  vdo_save_dirty_reference_blocks(refs);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Action wrapper to vdo_save_oldest_reference_block().
 **/
static void saveOldestReferenceBlockAction(struct vdo_completion *completion)
{
  vdo_save_oldest_reference_block(refs);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Verify that the new load code does, in fact, reproduce the original
 * reference counter.
 **/
static void verifyRefCountsLoad(void)
{
  struct vdo_slab *slabToLoad;
  VDO_ASSERT_SUCCESS(vdo_make_slab(FIRST_BLOCK, &allocator, 1, NULL, 0, false,
                                   &slabToLoad));
  VDO_ASSERT_SUCCESS(vdo_allocate_ref_counts_for_slab(slabToLoad));
  loaded = slabToLoad->reference_counts;
  performSuccessfulSlabAction(slabToLoad, VDO_ADMIN_STATE_SCRUBBING);
  CU_ASSERT_TRUE(vdo_are_equivalent_ref_counts(loaded, refs));
  CU_ASSERT_TRUE(vdo_are_equivalent_journal_points(&loaded->slab_journal_point,
                                                   &refs->slab_journal_point));
  for (block_count_t i = 0; i < refs->reference_block_count; i++) {
    struct reference_block *loadedBlock = &loaded->blocks[i];
    struct reference_block *refsBlock   = &refs->blocks[i];
    for (sector_count_t j = 0; j < VDO_SECTORS_PER_BLOCK; j++) {
      bool equivalent
        = vdo_are_equivalent_journal_points(&loadedBlock->commit_points[j],
                                            &refsBlock->commit_points[j]);
      CU_ASSERT_TRUE(equivalent);
    }
  }
  priority_table_remove(allocator.prioritized_slabs,
                        &slabToLoad->allocq_entry);
  vdo_free_slab(slabToLoad);
}

/**
 * Count the number of finished refcounts writes.
 *
 * <p>Implements VDOAction.
 **/
static void countFinishedWrites(struct vdo_completion *completion)
{
  runSavedCallbackAssertNoRequeue(completion);
  viosFinishedCount++;
  broadcast();
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfRefCountsBlockWrite(struct vdo_completion *completion)
{
  if (vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL)
      && isMetadataWrite(completion)
      && onBIOThread()
      && (pbnFromVIO(as_vio(completion)) < (slab->ref_counts_origin + 2))) {
    wrapCompletionCallback(completion, countFinishedWrites);
  }

  return true;
}

/**
 * Check whether the number of blocks finished writing is equal to the desired
 * number.
 *
 * Implements WaitCondition.
 **/
static bool isNumberFinishedCorrect(void *context)
{
  block_count_t *desiredCountPtr = (block_count_t *) context;
  return (*desiredCountPtr == viosFinishedCount);
}

/**
 * Test saving a single dirty block in a ref_counts object.
 **/
static void testWriteOne(void)
{
  // Touch an arbitary block.
  performSuccessfulAction(dirtyFirstBlockAction);

  desiredFinishedCount = 1;
  setCompletionEnqueueHook(wrapIfRefCountsBlockWrite);
  performSuccessfulAction(saveOldestReferenceBlockAction);

  // Wait for the VIO to finish.
  waitForCondition(isNumberFinishedCorrect, &desiredFinishedCount);
  clearCompletionEnqueueHooks();

  // We know the data is now safely on disk, so verify its correctness.
  verifyRefCountsLoad();
}

/**
 * Test saving two dirty blocks in a ref_counts object.
 **/
static void testWriteMany(void)
{
  desiredFinishedCount = 2;
  setCompletionEnqueueHook(wrapIfRefCountsBlockWrite);

  // Touch block 1.
  performSuccessfulAction(dirtyFirstBlockAction);

  // Touch a different block, hopefully block 2, unless the block state
  // information is greater than (VDO_BLOCK_SIZE / 2).
  performSuccessfulAction(dirtySecondBlockAction);
  performSuccessfulAction(saveDirtyBlocksAction);

  // Wait for both blocks to finish writing.
  waitForCondition(isNumberFinishedCorrect, &desiredFinishedCount);
  clearCompletionEnqueueHooks();
  verifyRefCountsLoad();
}

/**********************************************************************/
static enum reference_status
getExpectedStatus(physical_block_number_t blockNumber)
{
  unsigned int blockRefs = blockNumber % 255;
  switch (blockRefs) {
  case 0:
    return RS_FREE;

  case 1:
    return RS_SINGLE;

  default:
    return RS_SHARED;
  }
}

/**********************************************************************/
static void asyncSaveAndLoad(void)
{
  performSuccessfulSlabAction(refs->slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);
  verifyRefCountsLoad();

  block_count_t dataBlocks = depot->slab_config.data_blocks;
  for (physical_block_number_t pbn = FIRST_BLOCK; pbn < dataBlocks; pbn++) {
    assertAllocation(pbn);

    uint8_t refCount = (pbn % 255);
    switch (refCount) {
    case 0:
      // Release the provisional reference.
      assertAdjustment(pbn, NULL, false, RS_FREE);
      break;
    default:
      addManyReferences(pbn, refCount);
    }
  }

  performSuccessfulSlabAction(refs->slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);
  verifyRefCountsLoad();

  for (physical_block_number_t pbn = FIRST_BLOCK; pbn < dataBlocks; pbn++) {
    assertReferenceStatus(pbn, getExpectedStatus(pbn));
  }
}

/**
 * Test asynchronous save and load.
 **/
static void testAsyncSaveAndLoad(void)
{
  asyncSaveAndLoad();
}

/**
 * Implements BlockCondition.
 **/
static bool
shouldBlockVIO(struct vdo_completion *completion,
               void                  *context __attribute__((unused)))
{
  return (is_vio(completion)
          && (pbnFromVIO(as_vio(completion)) == pbnToBlock));
}

/**
 * Block the first VIO to a specific physical block number.
 *
 * @param pbn   The physical block number to catch
 **/
static void setupBlockLatch(physical_block_number_t pbn)
{
  pbnToBlock = pbn;
  setBlockVIOCompletionEnqueueHook(shouldBlockVIO, true);
}

/**
 * A wrapper around drainSlab() to save reference blocks.<p>
 *
 * Implements CloseLauncher.
 **/
static void saveRefBlocksWrapper(void *context, struct vdo_completion *parent)
{
  struct ref_counts *refCounts = context;
  if (vdo_start_draining(&refCounts->slab->state, VDO_ADMIN_STATE_SAVING,
                         parent, NULL)) {
    vdo_drain_ref_counts(refCounts);
  }
}

/**
 * A function to check if the refcounts thinks it's closed. Implements
 * ClosednessVerifier.
 **/
static bool checkRefCountsClosed(void *context)
{
  struct ref_counts *refCounts = context;
  return vdo_is_state_quiescent(&refCounts->slab->state);
}

/**
 * Release a blocked write. Implements BlockedIOReleaser.
 **/
static void releaseBlockedWrite(void *context)
{
  reallyEnqueueVIO(context);
}

/**
 * Test a block being updated while writing.
 **/
static void testBlockCollisions(void)
{
  // Catch the first write.
  setupBlockLatch(slab->ref_counts_origin);
  performSuccessfulAction(dirtyFirstBlockAction);

  // Kick off a dirty block write (to PBN 0).
  performSuccessfulAction(saveOldestReferenceBlockAction);
  struct vio *blocked = getBlockedVIO();

  // Update the same reference_block, but a different PBN in that block.
  performSuccessfulAction(redirtyFirstBlockAction);
  // Kick off a dirty block write (to PBN 0), while it is still in progress.
  // This should, theoretically, have no dirty blocks.
  performSuccessfulAction(saveOldestReferenceBlockAction);

  // Let the blocked write go.
  reallyEnqueueVIO(blocked);

  // Launch another one and wait for it to occur.
  setupBlockLatch(slab->ref_counts_origin);
  performSuccessfulAction(saveOldestReferenceBlockAction);
  releaseBlockedVIO();

  // Dirty the same reference_block, launch its save, and block it.
  setupBlockLatch(slab->ref_counts_origin);
  performSuccessfulAction(redirtyFirstBlockAction);
  performSuccessfulAction(saveOldestReferenceBlockAction);
  blocked = getBlockedVIO();

  // Update the same reference_block, but a different PBN in that block.
  performSuccessfulAction(redirtyFirstBlockAction);

  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = saveRefBlocksWrapper,
    .checker        = checkRefCountsClosed,
    .closeContext   = refs,
    .releaser       = releaseBlockedWrite,
    .releaseContext = blocked,
    .threadID       = allocator.thread_id,
  };

  runLatchedClose(closeInfo, VDO_SUCCESS);
  verifyRefCountsLoad();
}

/**********************************************************************/
static void doProvisionalReferencing(struct vdo_completion *completion)
{
  CU_ASSERT_PTR_EQUAL(&refs->blocks[0], refs->search_cursor.block);
  block_count_t firstRefBlockAllocatedCount  = refs->blocks[0].allocated_count;
  block_count_t secondRefBlockAllocatedCount = refs->blocks[1].allocated_count;
  physical_block_number_t pbn = FIRST_BLOCK + COUNTS_PER_BLOCK;
  VDO_ASSERT_SUCCESS(vdo_provisionally_reference_block(refs, pbn, NULL));
  CU_ASSERT_EQUAL(firstRefBlockAllocatedCount, refs->blocks[0].allocated_count);
  CU_ASSERT_EQUAL(secondRefBlockAllocatedCount + 1,
                  refs->blocks[1].allocated_count);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Make sure we bump the allocated count for the right block when
 * provisionally referencing.
 **/
static void testProvisionalForDedupe(void)
{
  block_count_t blockCount = vdo_get_unreferenced_block_count(refs);
  CU_ASSERT_TRUE(blockCount > 256);

  // Set the first reference block to non-zero reference counts.
  for (block_count_t i = 0; i < COUNTS_PER_BLOCK; i++) {
    addManyReferences(FIRST_BLOCK + i, (i % (MAXIMUM_REFERENCE_COUNT)) + 1);
  }

  // Try to provisionally reference the next block, refcount 0, and make sure
  // the right allocated count changes.
  performSuccessfulAction(doProvisionalReferencing);

  // Make sure we can save and load.
  performSuccessfulSlabAction(refs->slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  // Unset the provisional reference.
  assertAdjustment(FIRST_BLOCK + COUNTS_PER_BLOCK, NULL, false, RS_FREE);
  verifyRefCountsLoad();
}

/**
 * Clear provisional references in a slab full of such blocks.
 **/
static void testClearProvisional(void)
{
  block_count_t blockCount = vdo_get_unreferenced_block_count(refs);
  CU_ASSERT_TRUE(blockCount > 256);

  // Set the first 254 to all valid non-zero reference counts.
  for (block_count_t i = 0; i < 254; i++) {
    addManyReferences(FIRST_BLOCK + i, 1 + i);
  }

  // Set the rest to provisionally referenced.
  for (block_count_t i = 254; i < blockCount; i++) {
    assertAllocation(FIRST_BLOCK + i);
  }

  // Save this block with many provisional references.
  performSuccessfulSlabAction(refs->slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  // Unset the provisional references.
  for (block_count_t i = 254; i < blockCount; i++) {
    assertAdjustment(FIRST_BLOCK + i, NULL, false, RS_FREE);
  }

  // Loading it again should automatically clear the provisional references,
  // matching the adjustment just performed.
  verifyRefCountsLoad();
}

/**
 * Replay a reference count adjustment and check that the resulting count is
 * as expected.
 *
 * @param slabBlockNumber   The slab block number to adjust
 * @param slabJournalPoint  The point of slab journal entry for this adjustment
 * @param increment         Whether to increment or decrement the count
 * @param expectedCount     The expected reference count after the replay
 **/
static void assertReplay(slab_block_number           slabBlockNumber,
                         const struct journal_point *slabJournalPoint,
                         bool                        increment,
                         vdo_refcount_t              expectedCount)
{
  struct slab_journal_entry entry = {
    .sbn       = slabBlockNumber,
    .operation = (increment ? VDO_JOURNAL_DATA_INCREMENT
                            : VDO_JOURNAL_DATA_DECREMENT),
  };
  VDO_ASSERT_SUCCESS(vdo_replay_reference_count_change(loaded,
                                                       slabJournalPoint,
                                                       entry));
  CU_ASSERT_EQUAL(expectedCount, loaded->counters[slabBlockNumber]);
}

/**********************************************************************/
static void testReplay(void)
{
  struct journal_point point1 = {
    .sequence_number = 11,
    .entry_count     = 42,
  };
  struct journal_point point2 = {
    .sequence_number = point1.sequence_number,
    .entry_count     = point1.entry_count + 1,
  };
  struct journal_point point3 = {
    .sequence_number = point2.sequence_number,
    .entry_count     = point2.entry_count + 1,
  };
  CU_ASSERT_TRUE(vdo_before_journal_point(&point1, &point2));
  CU_ASSERT_TRUE(vdo_before_journal_point(&point2, &point3));

  slab_block_number       sbn = 0;
  physical_block_number_t pbn = FIRST_BLOCK + sbn;

  // Make the first incRef to the first block at the first point.
  assertAdjustment(pbn, &point1, true, RS_SINGLE);
  CU_ASSERT_EQUAL(1, refs->counters[sbn]);

  // Make the second incRef to the first block at the second point.
  assertAdjustment(pbn, &point2, true, RS_SHARED);

  // Save and load the reference counts so the commit point is updated.
  performSuccessfulSlabAction(refs->slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  struct vdo_slab *slabToLoad;
  VDO_ASSERT_SUCCESS(vdo_make_slab(FIRST_BLOCK, &allocator, 1, NULL, 0, false,
                                   &slabToLoad));
  VDO_ASSERT_SUCCESS(vdo_allocate_ref_counts_for_slab(slabToLoad));
  loaded = slabToLoad->reference_counts;
  performSuccessfulSlabAction(loaded->slab, VDO_ADMIN_STATE_SCRUBBING);

  // Pretend that a third adjustment, a decRef, was made at the third point,
  // but not committed. We crash, then all three entries are replayed.

  // Replay record 1 incRef: no-op (commit point minus one)
  assertReplay(sbn, &point1, true, 2);

  // Replay record 2 incRef: no-op (commit point boundary case)
  assertReplay(sbn, &point2, true, 2);

  // Replay record 3 decRef: replayed (commit point plus one)
  assertReplay(sbn, &point3, false, 1);

  vdo_free_slab(slabToLoad);
}

/**
 * Action wrapper to enter read-only mode.
 **/
static void enterReadOnlyModeAction(struct vdo_completion *completion)
{
  vdo_enter_read_only_mode(readOnlyNotifier, VDO_READ_ONLY);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Release blocked writes. Implements BlockedIOReleaser.
 **/
static void releaseBlockedWrites(void *context)
{
  struct vio **blockedVIOs = context;
  reallyEnqueueVIO(blockedVIOs[0]);
  reallyEnqueueVIO(blockedVIOs[1]);
}

/**
 * Test saving in read-only mode.
 **/
static void testReadOnly(void)
{
  // Catch the first write.
  setupBlockLatch(slab->ref_counts_origin);
  performSuccessfulAction(dirtyFirstBlockAction);
  performSuccessfulAction(saveOldestReferenceBlockAction);

  // Wait for it to be blocked.
  struct vio *blockedVIOs[2];
  blockedVIOs[0] = getBlockedVIO();

  performSuccessfulAction(redirtyFirstBlockAction);
  performSuccessfulAction(dirtySecondBlockAction);

  // Save the oldest (which is currently the second) reference block.
  setupBlockLatch(slab->ref_counts_origin + 1);
  performSuccessfulAction(saveOldestReferenceBlockAction);
  blockedVIOs[1] = getBlockedVIO();

  // Go into read-only mode while both blocks are writing.
  performSuccessfulAction(enterReadOnlyModeAction);

  // Assert saving won't finish until both blocks are finished writing.
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = saveRefBlocksWrapper,
    .checker        = checkRefCountsClosed,
    .closeContext   = refs,
    .releaser       = releaseBlockedWrites,
    .releaseContext = blockedVIOs,
    .threadID       = allocator.thread_id,
  };

  runLatchedClose(closeInfo, VDO_READ_ONLY);
}

/**********************************************************************/

static CU_TestInfo refCountsTests[] = {
  { "basic",                         testBasic                     },
  { "single block write",            testWriteOne                  },
  { "many block write",              testWriteMany                 },
  { "load/save refcounts",           testAsyncSaveAndLoad          },
  { "same-block busy update",        testBlockCollisions           },
  { "provisional for dedupe",        testProvisionalForDedupe      },
  { "clear provisionals",            testClearProvisional          },
  { "replay",                        testReplay                    },
  { "read-only",                     testReadOnly                  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo refCountsSuite = {
  .name                     = "reference counter tests (RefCounts_t1)",
  .initializer              = initializeRefCountsT1,
  .cleaner                  = tearDownRefCountsT1,
  .tests                    = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
