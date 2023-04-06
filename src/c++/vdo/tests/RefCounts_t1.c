/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "slab-depot.h"
#include "status-codes.h"
#include "vdo.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
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
  TEST_VIO_POOL_SIZE = 2,
};

struct vdo_slab                  *slab;
struct vdo_slab                  *loaded;
static physical_block_number_t    pbnToBlock;
static physical_block_number_t    firstBlock;
static physical_block_number_t    offset;
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
  vdo_finish_completion(parent);
}

/**********************************************************************/
static void initializeRefCountsT1(void)
{
  TestParameters testParameters = {
    .slabSize = SLAB_SIZE,
    .slabJournalBlocks = JOURNAL_SIZE,
    .slabCount = 1,
    .noIndexRegion = true,
  };
  initializeVDOTest(&testParameters);

  // This test assumes reference blocks are initialized to zero.
  slab = vdo->depot->slabs[0];
  zeroRAMLayer(getSynchronousLayer(),
               slab->ref_counts_origin,
               slab->end - slab->ref_counts_origin);

  expectedCloseResult = VDO_SUCCESS;
  VDO_ASSERT_SUCCESS(vdo_register_read_only_listener(vdo, NULL, readOnlyNotification, 0));
  viosFinishedCount          = 0;
  refCountsCompletionWaiting = false;
  firstBlock                 = slab->start;
  offset                     = firstBlock - 1;

  /*
   * Set the slab to be rebuilding so that slab journal locks will be ignored.
   * Since this test doesn't maintain the correct lock invariants, it would
   * fail on a lock count underflow otherwise.
   */
  slab->status = VDO_SLAB_REPLAYING;
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
  VDO_ASSERT_SUCCESS(getReferenceStatus(slab, pbn, &status));
  CU_ASSERT_EQUAL(expectedStatus, status);
}

/**
 * Perform a reference count adjustment and assert the return value.
 *
 * @param pbn                        The physical block number to adjust
 * @param slabJournalPoint           The journal point of the slab journal
 *                                   entry for this adjustment
 * @param operation                  The type of adjustment to perform
 * @param increment                  True if the adjustment is an increment
 * @param expectedResult             The expected result of the adjustment
 * @param expectedFreeStatusChanged  Whether the free status should change
 **/
static void
performAdjustment(physical_block_number_t     pbn,
                  const struct journal_point *slabJournalPoint,
                  enum journal_operation      operation,
                  bool                        increment,
                  int                         expectedResult,
                  bool                        expectedFreeStatusChanged)
{
  bool freeStatusChanged = ((expectedResult == VDO_SUCCESS)
                            ? !expectedFreeStatusChanged
                            : expectedFreeStatusChanged);
  struct reference_updater updater = {
    .operation = operation,
    .increment = increment,
    .zpbn = {
      .pbn = pbn,
    },
  };
  CU_ASSERT_EQUAL(adjust_reference_count(slab, &updater, slabJournalPoint, &freeStatusChanged),
                  expectedResult);
  CU_ASSERT_EQUAL(expectedFreeStatusChanged, freeStatusChanged);
}

/**
 * Adjust a reference count and check that the resulting status is as expected.
 *
 * @param pbn               The (test relative) physical block number to adjust
 * @param slabJournalPoint  The journal point of the slab journal entry for
 *                          this adjustment
 * @param operation         The type of adjustment
 * @param increment         Whether the adjustment is an increment or a
 *                          decrement
 * @param expectedStatus    The expected reference status after the adjustment
 **/
static void assertAdjustment(physical_block_number_t     pbn,
                             const struct journal_point *slabJournalPoint,
                             enum journal_operation      operation,
                             bool                        increment,
                             enum reference_status       expectedStatus)
{
  bool expectedFreeStatusChanged;
  if (expectedStatus == RS_FREE) {
    expectedFreeStatusChanged = !increment;
  } else {
    enum reference_status oldStatus;
    VDO_ASSERT_SUCCESS(getReferenceStatus(slab, pbn, &oldStatus));
    expectedFreeStatusChanged = ((oldStatus == RS_FREE) && increment);
  }

  block_count_t freeBefore = slab->free_blocks;
  performAdjustment(pbn,
                    slabJournalPoint,
                    operation,
                    increment,
                    VDO_SUCCESS,
                    expectedFreeStatusChanged);
  block_count_t freeAfter = slab->free_blocks;

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
  VDO_ASSERT_SUCCESS(allocate_slab_block(slab, &allocatedPBN));
  CU_ASSERT_EQUAL(expectedPBN, allocatedPBN);
}

/**********************************************************************/
static void assertFailedAdjustment(physical_block_number_t pbn,
                                   bool                    increment,
                                   int                     expectedResult)
{
  enum reference_status oldStatus;
  VDO_ASSERT_SUCCESS(getReferenceStatus(slab, pbn, &oldStatus));
  performAdjustment(pbn,
                    NULL,
                    VDO_JOURNAL_DATA_REMAPPING,
                    increment,
                    expectedResult,
                    false);
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
    struct reference_updater updater = {
      .operation = VDO_JOURNAL_DATA_REMAPPING,
      .increment = true,
      .zpbn      = {
        .pbn = pbn,
      },
    };
    VDO_ASSERT_SUCCESS(adjust_reference_count(slab, &updater, NULL, &freeStatusChanged));
  }
}

/**********************************************************************/
static void assertBlockMapIncrement(physical_block_number_t pbn)
{
  block_count_t freeBefore = slab->free_blocks;
  performAdjustment(pbn,
                    NULL,
                    VDO_JOURNAL_BLOCK_MAP_REMAPPING,
                    true,
                    VDO_SUCCESS,
                    false);
  assertReferenceStatus(pbn, RS_SHARED);
  // The block was already counted as not free when it was provisionally referenced.
  CU_ASSERT_EQUAL(freeBefore, slab->free_blocks);
  assertFailedAdjustment(pbn, true, VDO_REF_COUNT_INVALID);
}

/**********************************************************************/
static void resetReferenceCountsAction(struct vdo_completion *completion)
{
  resetReferenceCounts(slab);
  vdo_finish_completion(completion);
}

/**
 * Most basic refCounts test.
 **/
static void testBasic(void)
{
  enum reference_status refStatus;
  block_count_t         dataBlocks = vdo->depot->slab_config.data_blocks;
  physical_block_number_t pbns[7];

  for (physical_block_number_t pbn = firstBlock; pbn <= dataBlocks; pbn++) {
    assertReferenceStatus(pbn, RS_FREE);
    physical_block_number_t translated = pbn - offset;
    if (translated < 7) {
      pbns[translated] = pbn;
    }
  }

  CU_ASSERT_EQUAL(dataBlocks, slab->free_blocks);
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE, getReferenceStatus(slab, firstBlock - 1, &refStatus));
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE,
                  getReferenceStatus(slab, firstBlock + dataBlocks, &refStatus));

  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING,  true, RS_SINGLE);
  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING,  true, RS_SHARED);
  assertAdjustment(pbns[2], NULL, VDO_JOURNAL_DATA_REMAPPING,  true, RS_SINGLE);
  assertAdjustment(pbns[2], NULL, VDO_JOURNAL_DATA_REMAPPING,  true, RS_SHARED);
  assertAdjustment(pbns[2], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_SINGLE);
  assertAdjustment(pbns[2], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_FREE);
  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING,  true, RS_SHARED);
  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_SHARED);
  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_SINGLE);
  assertAdjustment(pbns[1], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_FREE);

  assertFailedDecrement(pbns[1]);

  assertAllocation(pbns[1]);
  CU_ASSERT_EQUAL(dataBlocks - 1, slab->free_blocks);
  assertReferenceStatus(pbns[1], RS_PROVISIONAL);

  assertAdjustment(pbns[3], NULL, VDO_JOURNAL_DATA_REMAPPING, true, RS_SINGLE);
  CU_ASSERT_EQUAL(dataBlocks - 2, slab->free_blocks);

  assertAllocation(pbns[2]);
  CU_ASSERT_EQUAL(dataBlocks - 3, slab->free_blocks);
  assertReferenceStatus(pbns[2], RS_PROVISIONAL);

  // Block #3 was manually incRef'ed, so it will be skipped and #4 allocated.
  assertAllocation(pbns[4]);
  CU_ASSERT_EQUAL(dataBlocks - 4, slab->free_blocks);
  assertReferenceStatus(pbns[4], RS_PROVISIONAL);
  assertAdjustment(pbns[4], NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_FREE);
  assertFailedDecrement(pbns[4]);

  addManyReferences(pbns[5], 254);
  assertReferenceStatus(pbns[5], RS_SHARED);

  assertFailedDecrement(pbns[6]);

  // Test block map increment succeeds for a provisionally referenced block.
  assertBlockMapIncrement(pbns[1]);

  // Test block map increments fail for RS_FREE.
  performAdjustment(pbns[4],
                    NULL,
                    VDO_JOURNAL_BLOCK_MAP_REMAPPING,
                    true,
                    VDO_REF_COUNT_INVALID,
                    false);
  // Test block map increments fail for RS_SINGLE.
  performAdjustment(pbns[3],
                    NULL,
                    VDO_JOURNAL_BLOCK_MAP_REMAPPING,
                    true,
                    VDO_REF_COUNT_INVALID,
                    false);
  // Test block map increments fail for RS_SHARED.
  assertAdjustment(pbns[3], NULL, VDO_JOURNAL_DATA_REMAPPING, true, RS_SHARED);
  performAdjustment(pbns[3],
                    NULL,
                    VDO_JOURNAL_BLOCK_MAP_REMAPPING,
                    true,
                    VDO_REF_COUNT_INVALID,
                    false);

  // Enter read only mode so that the ref counts don't need to be drained on tear down.
  performSuccessfulActionOnThread(resetReferenceCountsAction, slab->allocator->thread_id);
}

/**
 * Action wrapper to modify first refcount on first block.
 **/
static void dirtyFirstBlockAction(struct vdo_completion *completion)
{
  addManyReferences(firstBlock, 1);
  vdo_finish_completion(completion);
}

/**
 * Action wrapper to modify second refcount on first block.
 **/
static void redirtyFirstBlockAction(struct vdo_completion *completion)
{
  addManyReferences(firstBlock + 1, 1);
  vdo_finish_completion(completion);
}

/**
 * Action wrapper to modify a refcount on the second block.
 **/
static void dirtySecondBlockAction(struct vdo_completion *completion)
{
  addManyReferences(firstBlock + VDO_BLOCK_SIZE, 1);
  vdo_finish_completion(completion);
}

/**
 * Action wrapper to fire off all dirty blocks.
 **/
static void saveDirtyBlocksAction(struct vdo_completion *completion)
{
  // Fire off every dirty reference block in the queue at once.
  vdo_save_dirty_reference_blocks(slab);
  vdo_finish_completion(completion);
}

/**
 * Action wrapper to save a reference block.
 **/
static void saveOldestReferenceBlockAction(struct vdo_completion *completion)
{
  vdo_notify_next_waiter(&slab->dirty_blocks, launch_reference_block_write, slab);
  vdo_finish_completion(completion);
}

/**
 * Verify that the new load code does, in fact, reproduce the original
 * reference counter.
 **/
static void verifyRefCountsLoad(void)
{
  VDO_ASSERT_SUCCESS(make_slab(slab->start, slab->allocator, NULL, 0, false, &loaded));
  VDO_ASSERT_SUCCESS(vdo_allocate_slab_counters(loaded));
  performSuccessfulSlabAction(loaded, VDO_ADMIN_STATE_SCRUBBING);
  CU_ASSERT_TRUE(slabsHaveEquivalentReferenceCounts(loaded, slab));
  CU_ASSERT_TRUE(areJournalPointsEqual(loaded->slab_journal_point, slab->slab_journal_point));
  for (block_count_t i = 0; i < slab->reference_block_count; i++) {
    struct reference_block *loadedBlock = loaded->reference_blocks + i;
    struct reference_block *refsBlock   = slab->reference_blocks + i;
    for (sector_count_t j = 0; j < VDO_SECTORS_PER_BLOCK; j++) {
      CU_ASSERT_TRUE(areJournalPointsEqual(loadedBlock->commit_points[j],
                                           refsBlock->commit_points[j]));
    }
  }

  vdo_priority_table_remove(loaded->allocator->prioritized_slabs, &loaded->allocq_entry);
  free_slab(loaded);
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
  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);
  verifyRefCountsLoad();

  block_count_t dataBlocks = vdo->depot->slab_config.data_blocks;
  for (physical_block_number_t pbn = firstBlock; pbn < dataBlocks; pbn++) {
    assertAllocation(pbn);

    uint8_t refCount = (pbn % 255);
    switch (refCount) {
    case 0:
      // Release the provisional reference.
      assertAdjustment(pbn, NULL, VDO_JOURNAL_DATA_REMAPPING, false, RS_FREE);
      break;
    default:
      addManyReferences(pbn, refCount);
    }
  }

  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);
  verifyRefCountsLoad();

  for (physical_block_number_t pbn = firstBlock; pbn < dataBlocks; pbn++) {
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
  struct vdo_slab *slab = context;
  if (vdo_start_draining(&slab->state, VDO_ADMIN_STATE_SAVING, parent, NULL)) {
    drain_slab(slab);
  }
}

/**
 * A function to check if the refcounts thinks it's closed. Implements
 * ClosednessVerifier.
 **/
static bool checkRefCountsClosed(void *context)
{
  struct vdo_slab *slab = context;
  return vdo_is_state_quiescent(&slab->state);
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
    .closeContext   = slab,
    .releaser       = releaseBlockedWrite,
    .releaseContext = blocked,
    .threadID       = vdo->depot->allocators[0].thread_id,
  };

  runLatchedClose(closeInfo, VDO_SUCCESS);
  verifyRefCountsLoad();

  setStartStopExpectation(VDO_INVALID_ADMIN_STATE);
}

/**********************************************************************/
static void doProvisionalReferencing(struct vdo_completion *completion)
{
  CU_ASSERT_PTR_EQUAL(&slab->reference_blocks[0], slab->search_cursor.block);
  block_count_t firstRefBlockAllocatedCount  = slab->reference_blocks[0].allocated_count;
  block_count_t secondRefBlockAllocatedCount = slab->reference_blocks[1].allocated_count;
  physical_block_number_t pbn = firstBlock + COUNTS_PER_BLOCK;
  VDO_ASSERT_SUCCESS(vdo_acquire_provisional_reference(slab, pbn, NULL));
  CU_ASSERT_EQUAL(firstRefBlockAllocatedCount, slab->reference_blocks[0].allocated_count);
  CU_ASSERT_EQUAL(secondRefBlockAllocatedCount + 1, slab->reference_blocks[1].allocated_count);
  vdo_finish_completion(completion);
}

/**
 * Make sure we bump the allocated count for the right block when
 * provisionally referencing.
 **/
static void testProvisionalForDedupe(void)
{
  block_count_t blockCount = slab->free_blocks;
  CU_ASSERT_TRUE(blockCount > 256);

  // Set the first reference block to non-zero reference counts.
  for (block_count_t i = 0; i < COUNTS_PER_BLOCK; i++) {
    addManyReferences(firstBlock + i, (i % (MAXIMUM_REFERENCE_COUNT)) + 1);
  }

  // Try to provisionally reference the next block, refcount 0, and make sure
  // the right allocated count changes.
  performSuccessfulAction(doProvisionalReferencing);

  // Make sure we can save and load.
  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  // Unset the provisional reference.
  assertAdjustment(firstBlock + COUNTS_PER_BLOCK,
                   NULL,
                   VDO_JOURNAL_DATA_REMAPPING,
                   false,
                   RS_FREE);
  verifyRefCountsLoad();
}

/**
 * Clear provisional references in a slab full of such blocks.
 **/
static void testClearProvisional(void)
{
  block_count_t blockCount = slab->free_blocks;
  CU_ASSERT_TRUE(blockCount > 256);

  // Set the first 254 to all valid non-zero reference counts.
  for (block_count_t i = 0; i < 254; i++) {
    addManyReferences(firstBlock + i, 1 + i);
  }

  // Set the rest to provisionally referenced.
  for (block_count_t i = 254; i < blockCount; i++) {
    assertAllocation(firstBlock + i);
  }

  // Save this block with many provisional references.
  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  // Unset the provisional references.
  for (block_count_t i = 254; i < blockCount; i++) {
    assertAdjustment(firstBlock + i,
                     NULL,
                     VDO_JOURNAL_DATA_REMAPPING,
                     false,
                     RS_FREE);
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
    .increment = increment,
    .operation = VDO_JOURNAL_DATA_REMAPPING,
  };
  VDO_ASSERT_SUCCESS(replay_reference_count_change(loaded, slabJournalPoint, entry));
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
  physical_block_number_t pbn = firstBlock + sbn;

  // Make the first incRef to the first block at the first point.
  assertAdjustment(pbn, &point1, VDO_JOURNAL_DATA_REMAPPING, true, RS_SINGLE);
  CU_ASSERT_EQUAL(1, slab->counters[sbn]);

  // Make the second incRef to the first block at the second point.
  assertAdjustment(pbn, &point2, VDO_JOURNAL_DATA_REMAPPING, true, RS_SHARED);

  // Save and load the reference counts so the commit point is updated.
  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  VDO_ASSERT_SUCCESS(make_slab(firstBlock, slab->allocator, NULL, 0, false, &loaded));
  VDO_ASSERT_SUCCESS(vdo_allocate_slab_counters(loaded));
  performSuccessfulSlabAction(loaded, VDO_ADMIN_STATE_SCRUBBING);
  CU_ASSERT_TRUE(slabsHaveEquivalentReferenceCounts(loaded, slab));

  // Pretend that a third adjustment, a decRef, was made at the third point,
  // but not committed. We crash, then all three entries are replayed.

  // Replay record 1 incRef: no-op (commit point minus one)
  assertReplay(sbn, &point1, true, 2);

  // Replay record 2 incRef: no-op (commit point boundary case)
  assertReplay(sbn, &point2, true, 2);

  // Replay record 3 decRef: replayed (commit point plus one)
  assertReplay(sbn, &point3, false, 1);

  free_slab(loaded);
}

/**
 * Action wrapper to enter read-only mode.
 **/
static void enterReadOnlyModeAction(struct vdo_completion *completion)
{
  vdo_enter_read_only_mode(vdo, VDO_READ_ONLY);
  vdo_finish_completion(completion);
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
    .closeContext   = slab,
    .releaser       = releaseBlockedWrites,
    .releaseContext = blockedVIOs,
    .threadID       = vdo->depot->allocators[0].thread_id,
  };

  runLatchedClose(closeInfo, VDO_READ_ONLY);
  setStartStopExpectation(VDO_READ_ONLY);
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
  .cleaner                  = tearDownVDOTest,
  .tests                    = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
