/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "recovery-journal.h"
#include "slab-depot.h"
#include "status-codes.h"
#include "vdo.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "completionUtils.h"
#include "ioRequest.h"
#include "latchUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "recoveryModeUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  logical_block_number_t lbn;
  size_t                 data;
  block_count_t          size;
  bool                   written;
} DataSet;

enum {
  DEFAULT_MAPPABLE = 750,
  INJECTED_ERROR   = VDO_STATUS_CODE_LAST + 1,
};

/*
 * For the four variants of testRecoveryMode, there are six data-sets written:
 *
 * A: Write about a third of mappableBlocks, to set things up and establish
 *    slabs that will need to be recovered. This should be enough so the last
 *    slab scrubbed is full, and should also be at least three slabs even with
 *    compression on.
 *
 *    The tests then crash the VDO. After we come back up, we will start
 *    compressing for the tests which require it. To accommodate compression
 *    mode enabled, we will only write data which requires an integer multiple
 *    of VDO_MAX_COMPRESSION_SLOTS of new blocks to be allocated.
 *
 * B: Write some blocks while in recovery mode. The data will not be any
 *    blocks we've already written. The actual number of blocks written is
 *    VDO_MAX_COMPRESSION_SLOTS (to enable them to complete when
 *    compression is on).
 *
 * C: Write a larger number of blocks (7 * VDO_MAX_COMPRESSION_SLOTS) with data
 *    from the middle of the set established by A. These should not
 *    dedupe because no slab containing that data has not yet been scrubbed.
 *
 * D: Write the same data and same number of blocks as B. These should all
 *    dedupe.
 *
 * E: Latch the last slab scrubbing and write the same data as A (minus one
 *    slab worth), which would dedupe against A and C.
 *
 * F: Write a slab worth of data that would have deduped if the referenced slab
 *    had been scrubbed.
 *
 * G: Write some new data while in recovery mode.
 */
static DataSet testDataSets[] = {
  // {lbn, data, size, written}
  {     0,    1,  256, false }, // A: about a third of physical available
  {   300,  255,   14, false }, // B: some arbitrary number of blocks
  {   400,  101,   98, false }, // C: part of A, but won't dedupe
  {   500,  255,   14, false }, // D: same as B, and will dedupe
  {   600,    1,  240, false }, // E: dedupes against A and C
  {   900,  241,   14, false }, // F: dedupes against unrecovered slab
  {  1000,  269,   14, false }, // G: some arbitrary number of blocks
};

static logical_block_number_t zeroBlockLBN = 1100;
static bool                   waiterQueued;
static bool                   checkForWaiter;
static bool                   isInRecoveryMode;
static block_count_t          dataPerSlab;
static slab_count_t           totalSlabs;
static slab_count_t           slabToLatch;
static slab_count_t           inspectSlabNumber;
static struct vdo_slab       *latchedSlab;
static struct slab_scrubber  *scrubber;
static bool                   scrubberSuspending;
static slab_count_t           slabsToScrub;

/**********************************************************************/
static block_count_t getBlocksAllocated(void)
{
  return vdo_get_physical_blocks_allocated(vdo);
}

/**
 * Test-specific initialization.
 *
 * @param mappableBlocks  The number of mappable blocks
 **/
static void initializeRecoveryModeT1(block_count_t mappableBlocks)
{
  // Make a VDO with 4 block map pages, each of a different root, so
  // filling out the tree uses up exactly one single slab.
  const TestParameters parameters = {
    .mappableBlocks      = 16,
    .journalBlocks       = 32,
    .slabSize            = 32,
    .slabJournalBlocks   = 8,
    .logicalThreadCount  = 1,
    .physicalThreadCount = 1,
    .hashZoneThreadCount = 1,
    .logicalBlocks       = 2500,
  };

  initializeRecoveryModeTest(&parameters);

  // Initialize all the important parts of the block map tree.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 32);
  populateBlockMapTree();
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 16);
  restartVDO(false);
  dataPerSlab = vdo->depot->slab_config.data_blocks;

  /*
   * Test parameters requires we create a VDO with at least one slab free
   * after fully populating the block map, so to get N slabs for data we
   * we now add N-1 slabs.
   */
  slab_count_t dataSlabs = (mappableBlocks + dataPerSlab - 1) / dataPerSlab;
  addSlabs(dataSlabs - 1);

  // The resume which happened in addSlabs() reordered the priority table.
  // Restarting the VDO restores the ordering the test depends upon.
  restartVDO(false);
  totalSlabs = vdo->depot->slab_count;

  DataSet *lastDataSet = &testDataSets[ARRAY_SIZE(testDataSets) - 1];
  CU_ASSERT_TRUE(zeroBlockLBN > (lastDataSet->lbn + lastDataSet->size));
  for (size_t i = 0; i < (sizeof(testDataSets) / sizeof(DataSet)); i++) {
    testDataSets[i].written = false;
  }

  inspectSlabNumber = 0;
  slabToLatch       = totalSlabs;
  latchedSlab       = NULL;
}

/**
 * Write blocks one slab at a time.
 **/
static void writeBlocksSlabwise(logical_block_number_t lbn,
                                block_count_t          index,
                                block_count_t          count,
                                int                    expectedResult)
{
  while (count > 0) {
    writeData(lbn, index,
              ((count > dataPerSlab) ? dataPerSlab : count), expectedResult);
    lbn += dataPerSlab;
    index += dataPerSlab;
    count -= dataPerSlab;
  }
}

/**
 * Write a data set.
 **/
static void writeDataSet(size_t        dataSetNumber,
                         block_count_t expectedBlocksAllocated)
{
  DataSet *dataSet = &testDataSets[dataSetNumber];
  writeData(dataSet->lbn, dataSet->data, dataSet->size, VDO_SUCCESS);
  dataSet->written = true;
  verifyData(dataSet->lbn, dataSet->data, dataSet->size);
  CU_ASSERT_EQUAL(expectedBlocksAllocated, getBlocksAllocated());
}

/**
 * Verify the number of logical blocks used.
 **/
static void verifyLogicalBlockUsed(block_count_t expectedLogicalUsed)
{
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.logical_blocks_used, expectedLogicalUsed);
}

/**
 * Verify all written datasets.
 **/
static void verifyDataSets(void)
{
  for (size_t i = 0; i < (sizeof(testDataSets) / sizeof(DataSet)); i++) {
    if (testDataSets[i].written) {
      verifyData(testDataSets[i].lbn, testDataSets[i].data,
                 testDataSets[i].size);
    }
  }
}

/**
 * Write a zero block, checking that the block usage hasn't changed.
 **/
static void writeSingleZeroBlock(void)
{
  writeAndVerifyData(zeroBlockLBN, 0, 1, getPhysicalBlocksFree(),
                     getBlocksAllocated());
  zeroBlockLBN++;
}

/**
 * Action to check whether the VDO is in recovery mode.
 *
 * @param completion  The action completion
 **/
static void checkRecoveryMode(struct vdo_completion *completion)
{
  isInRecoveryMode = vdo_in_recovery_mode(vdo);
  vdo_finish_completion(completion);
}

/**
 * Check whether the VDO is in recovery mode.
 *
 * @return <code>true</code> if the VDO is in recovery mode
 **/
static bool checkInRecovery(void)
{
  performSuccessfulActionOnThread(checkRecoveryMode, 0);
  return isInRecoveryMode;
}

/**
 * Action to check that a slab will be scrubbed.
 **/
static void assertSlabNeedsScrubbing(struct vdo_completion *completion)
{
  CU_ASSERT_NOT_EQUAL(vdo->depot->slabs[slabToLatch]->status,
                      VDO_SLAB_REBUILT);
  vdo_finish_completion(completion);
}

/**
 * Check that the slab to latch will in fact be scrubbed.
 **/
static void checkSlabNeedsScrubbing(void)
{
  struct vdo_slab *slab = vdo->depot->slabs[slabToLatch];
  performSuccessfulActionOnThread(assertSlabNeedsScrubbing,
                                  slab->allocator->thread_id);
}

/**
 * Start the VDO and wait for it to go into recovery mode by latching a
 * slab that is being scrubbed.
 **/
static void
startAndWaitForVDOInRecovery(bool compress, enum vdo_state expectedState)
{
  if (slabToLatch != totalSlabs) {
    setupSlabScrubbingLatch(slabToLatch);
  } else {
    latchAnyScrubbingSlab(totalSlabs);
  }

  startVDO(expectedState);
  modifyCompressDedupe(compress, true);

  if (slabToLatch != totalSlabs) {
    checkSlabNeedsScrubbing();
  }

  struct slab_depot *depot       = vdo->depot;
  bool               slabLatched = false;
  latchedSlab                    = NULL;
  while (!checkInRecovery() || !slabLatched) {
    if (latchedSlab != NULL) {
      releaseSlabLatch(latchedSlab->slab_number);
    }
    latchedSlab = depot->slabs[waitForAnySlabToLatch(totalSlabs)];
    slabLatched = ((latchedSlab->slab_number == slabToLatch)
                   || (totalSlabs == slabToLatch));
  }
}

/**
 * Action to stop the slab scrubber and release all latched slabs.
 **/
static void stopScrubbingCallback(struct vdo_completion *completion)
{
  struct block_allocator *allocator = latchedSlab->allocator;
  vdo_prepare_completion(&allocator->completion,
                         finishParentCallback,
                         finishParentCallback,
                         allocator->thread_id,
                         completion);
  stop_scrubbing(allocator);
  releaseAllSlabLatches(totalSlabs);
  latchedSlab = NULL;
}

/**
 * Stop the slab scrubber and then release slab latches. The mutex must be
 * held while calling this method.
 *
 * Implements LockedMethod.
 **/
static bool
stopScrubberAndReleaseSlabs(void *context __attribute__((unused)))
{
  if (latchedSlab != NULL) {
    performSuccessfulActionOnThread(stopScrubbingCallback,
                                    latchedSlab->allocator->thread_id);
  }
  return false;
}

/**
 * Test writing data during VDO recovery mode.
 **/
static void testRecoveryMode(bool compress)
{
  initializeRecoveryModeT1(DEFAULT_MAPPABLE);

  block_count_t expectedAllocated   = 0;
  block_count_t expectedLogicalUsed = 0;
  block_count_t compressionFactor
    = (compress) ? VDO_MAX_COMPRESSION_SLOTS : 1;

  // Unique data write to fill half the physical space.
  // (Compression is always off for this.)
  expectedLogicalUsed++;
  writeSingleZeroBlock();
  expectedAllocated   += testDataSets[0].size;
  expectedLogicalUsed += testDataSets[0].size;
  writeBlocksSlabwise(testDataSets[0].lbn, testDataSets[0].data,
                      testDataSets[0].size, VDO_SUCCESS);
  verifyLogicalBlockUsed(expectedLogicalUsed);

  crashVDO();
  startAndWaitForVDOInRecovery(compress, VDO_DIRTY);

  // During recovery, unrecovered slabs are considered allocated entirely.
  expectedAllocated    = getBlocksAllocated();
  expectedAllocated   += testDataSets[1].size / compressionFactor;
  expectedLogicalUsed += testDataSets[1].size;
  // Reads and writes can be performed during recovery.
  writeDataSet(1, expectedAllocated);
  verifyDataSets();

  // Dedupe does not occur against unrecovered slabs.
  expectedAllocated   += testDataSets[2].size / compressionFactor;
  expectedLogicalUsed += testDataSets[2].size;
  writeDataSet(2, expectedAllocated);

  /*
   * We have a dilemma. We want to stop the scrubber, which requires releasing
   * the latched slab, but we don't want to release the latch until we know the
   * scrubber won't race with us to scrub more slabs before it is told to stop.
   * Furthermore, calls to vdo_stop_slab_scrubbing() are no longer idempotent.
   * So instead we need an action which will tell the scrubber to stop and then
   * release the latch.
   */
  runLocked(stopScrubberAndReleaseSlabs, NULL);
  stopVDO();

  // The VDO has been shut down while still in recovery mode.
  startAndWaitForVDOInRecovery(compress, VDO_RECOVERING);

  // Keep latching slabs until all but one have been scrubbed.
  uint8_t expectedProgress = (totalSlabs - 1) * 100 / totalSlabs;
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  slab_count_t latchedSlab = totalSlabs;
  while (stats.recovery_percentage < expectedProgress) {
    if (latchedSlab < totalSlabs) {
      releaseSlabLatch(latchedSlab);
    }
    latchedSlab = waitForAnySlabToLatch(totalSlabs);
    vdo_fetch_statistics(vdo, &stats);
  }
  CU_ASSERT_EQUAL(stats.recovery_percentage, expectedProgress);

  expectedAllocated = getBlocksAllocated();

  // Dedupe does occur against data we wrote in recovery mode.
  expectedLogicalUsed += testDataSets[3].size;
  writeDataSet(3, expectedAllocated);

  // Dedupe against the originally written data partially works.

  // This dataset will fully dedupe against C. The part which is not in C will
  // attempt to dedupe against parts of A in scrubbed slabs.
  expectedLogicalUsed += testDataSets[4].size;
  writeDataSet(4, expectedAllocated);

  // This dataset does not dedupe due to the unrecovered slab.
  expectedAllocated   += testDataSets[5].size / compressionFactor;
  expectedLogicalUsed += testDataSets[5].size;
  writeDataSet(5, expectedAllocated);

  // Reads and writes with new data can be performed during recovery.
  expectedAllocated   += testDataSets[6].size / compressionFactor;
  expectedLogicalUsed += testDataSets[6].size;
  writeDataSet(6, expectedAllocated);
  verifyDataSets();

  // Release the latch set in startAndWaitForVDOInRecovery so that the
  // VDO exits recovery mode.
  releaseAllSlabLatches(totalSlabs);
  waitForRecoveryDone();

  // Statistics should be correct upon leaving recovery mode.
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.recovery_percentage, 100);
  verifyLogicalBlockUsed(expectedLogicalUsed);

  expectedAllocated = (testDataSets[1].size + testDataSets[2].size
                       + testDataSets[5].size + testDataSets[6].size)
                       / compressionFactor;
  expectedAllocated += testDataSets[0].size;
  CU_ASSERT_EQUAL(getBlocksAllocated(), expectedAllocated);
  verifyDataSets();
}

/**********************************************************************/
static void testRecoveryModeNoCompress(void)
{
  testRecoveryMode(false);
}

/**********************************************************************/
static void testRecoveryCompress(void)
{
  testRecoveryMode(true);
}

/**
 * Amazingly, this is a safe use of a callback finished hook.
 **/
static void triggerWaiterCheck(void)
{
  signalState(&checkForWaiter);
}

/**
 * Launch a write and then repeatedly perform a supplied action until
 * the write's data vio is blocked.
 *
 * @param start   The logical block at which to start writing
 * @param offset  The offset into data of the first block
 * @param action  The action to apply
 *
 * @return The blocked vio
 **/
static IORequest *waitForVIOWaiting(logical_block_number_t  start,
                                    block_count_t           offset,
                                    vdo_action_fn           action)
{
  // Prepare to wait for the next write to block in the scrubber
  waiterQueued = false;
  checkForWaiter = true;
  setCallbackFinishedHook(triggerWaiterCheck);

  // Launch a write which will wait to be scrubbed.
  IORequest       *request = launchIndexedWrite(start, 1, offset);
  struct vdo_slab *slab    = vdo->depot->slabs[slabToLatch];

  while (!waiterQueued) {
    waitForStateAndClear(&checkForWaiter);
    performSuccessfulActionOnThread(action, slab->allocator->thread_id);
  }

  return request;
}

/**
 * Action to check whether slab 1 has waiters.
 **/
static void checkSlabWaiters(struct vdo_completion *completion)
{
  struct vdo_slab *slab = vdo->depot->slabs[slabToLatch];
  if (vdo_waitq_has_waiters(&slab->allocator->scrubber.waiters)) {
    waiterQueued = true;
    setCallbackFinishedHook(NULL);
  }

  vdo_finish_completion(completion);
}

/**
 * Make a VDO, crash it, restart it, latch slab 2, and then launch a write and
 * wait for it to block.
 *
 * @param lbnPtr  A pointer to hold the lbn of the blocked write (may be NULL)
 *
 * @return The blocked request
 **/
static IORequest *prepareFreeSpaceWait(logical_block_number_t *lbnPtr)
{
  initializeRecoveryModeT1(DEFAULT_MAPPABLE);
  block_count_t totalFreeBlocks = getPhysicalBlocksFree();
  writeBlocksSlabwise(1, 1, totalFreeBlocks, VDO_SUCCESS);

  // Zero write a block from the first and second slabs.
  writeData(1, 0, 1, VDO_SUCCESS);
  writeData(1 + dataPerSlab, 0, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 2);

  crashVDO();

  // Restart and wait until slab 2 is latched
  slabToLatch = 2;
  startAndWaitForVDOInRecovery(false, VDO_DIRTY);

  // Write a new data block.
  logical_block_number_t newLBN = totalFreeBlocks + 1;
  writeData(newLBN, newLBN, 1, VDO_SUCCESS);
  newLBN++;
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);
  if (lbnPtr != NULL) {
    *lbnPtr = newLBN;
  }

  return waitForVIOWaiting(newLBN, newLBN, checkSlabWaiters);
}

/**
 * Test VIOs waiting on unrecovered slabs to be scrubbed if VDO has no space
 * during recovery mode.
 **/
static void testFreeSpaceWait(void)
{
  // Launch two writes with new data. Both wait for slab 1 to be scrubbed.
  logical_block_number_t lbn;
  IORequest *firstWrite = prepareFreeSpaceWait(&lbn);
  lbn++;
  IORequest *secondWrite = launchIndexedWrite(lbn, 1, lbn);

  // Let scrubbing finish
  releaseAllSlabLatches(totalSlabs);

  // The first write used the only free block in slab 1.
  awaitAndFreeSuccessfulRequest(vdo_forget(firstWrite));

  // The second write failed because there is no space in the VDO.
  CU_ASSERT_EQUAL(awaitAndFreeRequest(vdo_forget(secondWrite)), VDO_NO_SPACE);
}

/**
 * Action to check whether a slab's journal has waiters.
 **/
static void checkSlabJournalWaiters(struct vdo_completion *completion)
{
  struct slab_journal *journal = &vdo->depot->slabs[slabToLatch]->journal;
  if (vdo_waitq_has_waiters(&journal->entry_waiters)) {
    waiterQueued = true;
    setCallbackFinishedHook(NULL);
  }

  vdo_finish_completion(completion);
}

/**
 * Test that VIOs which were waiting on slab scrubbing do not hang when the
 * VDO enters read-only mode.
 **/
static void testSlabScrubbingErrorHang(void)
{
  IORequest *request = prepareFreeSpaceWait(NULL);

  // Write zeros to a block in the slab being scrubbed
  IORequest *request2
    = waitForVIOWaiting(dataPerSlab + 2, 0, checkSlabJournalWaiters);

  injectErrorInLatchedSlab(slabToLatch, INJECTED_ERROR);

  // Let it all go
  releaseSlabLatch(slabToLatch);
  CU_ASSERT_EQUAL(awaitAndFreeRequest(vdo_forget(request)), VDO_READ_ONLY);
  CU_ASSERT_EQUAL(awaitAndFreeRequest(vdo_forget(request2)), VDO_READ_ONLY);
  setStartStopExpectation(VDO_READ_ONLY);
}

/**
 * Test that an unrecovered slab will be made high-priority if VIOs need to
 * make slab journal entries, but there isn't space to do so.
 **/
static void testRequeueUnrecoveredSlab(void)
{
  initializeRecoveryModeT1(DEFAULT_MAPPABLE);
  block_count_t totalFreeBlocks = getPhysicalBlocksFree();
  writeBlocksSlabwise(dataPerSlab, 1, totalFreeBlocks, VDO_SUCCESS);

  crashVDO();

  // vdo_slab 1 is scrubbed before coming online and slab 2 is the first slab
  // scrubbed during recovery mode.
  slabToLatch = 2;
  startAndWaitForVDOInRecovery(false, VDO_DIRTY);

  // Get the last slab in the scrubber
  struct block_allocator *allocator = &vdo->depot->allocators[0];
  struct slab_scrubber   *scrubber  = &allocator->scrubber;
  struct vdo_slab        *slab
    = list_last_entry(&scrubber->slabs, struct vdo_slab, allocq_entry);
  CU_ASSERT_NOT_EQUAL(slab->slab_number, slabToLatch);

  // Shorten the slab journal blocking threshold.
  struct slab_journal *journal      = &slab->journal;
  block_count_t        oldThreshold = journal->blocking_threshold;
  journal->blocking_threshold       = 0;

  // Launch a zero write in the last slab and wait for the VIO to be enqueued
  // on that slab's journal.
  slabToLatch = slab->slab_number;
  logical_block_number_t lbn = slabToLatch * dataPerSlab;
  IORequest *request = waitForVIOWaiting(lbn, 0, checkSlabJournalWaiters);
  journal->blocking_threshold = oldThreshold;

  // Verify that the slab has become high priority and is on the correct queue
  CU_ASSERT_EQUAL(slab->status, VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING);
  CU_ASSERT_PTR_EQUAL(&slab->allocq_entry, scrubber->high_priority_slabs.next);

  // Release the reference block write to allow slabs to be scrubbed and wait
  // for the trim to finish.
  releaseAllSlabLatches(totalSlabs);
  awaitAndFreeSuccessfulRequest(vdo_forget(request));
}

/**
 * An action to check whether the slab scrubber is suspending.
 **/
static void checkForSuspending(struct vdo_completion *completion)
{
  if (vdo_is_state_suspending(&scrubber->admin_state)) {
    signalState(&scrubberSuspending);
  }

  vdo_finish_completion(completion);
}

/**
 * An action to check whether the scrubber has any slabs to scrub.
 **/
static void countUnscrubbedSlabs(struct vdo_completion *completion)
{
  slabsToScrub = READ_ONCE(scrubber->slab_count);
  vdo_finish_completion(completion);
}

/**
 * Test that suspending and resuming a VDO which is still scrubbing correctly
 * restarts the scrubber and scrubs all the slabs.
 **/
static void testSuspendAndResumeWhileScrubbing(void)
{
  initializeRecoveryModeT1(DEFAULT_MAPPABLE);
  block_count_t totalFreeBlocks = getPhysicalBlocksFree();
  writeBlocksSlabwise(dataPerSlab, 1, totalFreeBlocks, VDO_SUCCESS);

  crashVDO();

  // vdo_slab 0 is scrubbed before coming online and slab 1 is the first slab
  // scrubbed during recovery mode.
  //  slabToLatch = 1;
  startAndWaitForVDOInRecovery(false, VDO_DIRTY);

  struct slab_depot *depot = vdo->depot;
  scrubber                 = &depot->allocators[0].scrubber;
  scrubberSuspending       = false;

  // Tell the depot to suspend and then release the slab latch so the suspend
  // can actually happen.
  struct vdo_completion *completion
    = launchDepotAction(depot, VDO_ADMIN_STATE_SUSPENDING);
  while (!checkState(&scrubberSuspending)) {
    performSuccessfulActionOnThread(checkForSuspending, depot->allocators[0].thread_id);
  }

  releaseSlabLatch(latchedSlab->slab_number);
  VDO_ASSERT_SUCCESS(awaitCompletion(completion));
  vdo_free(completion);

  performSuccessfulAction(countUnscrubbedSlabs);

  // Resume the depot.
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RESUMING);

  // Make sure all slabs are scrubbed and that we exit recovery mode
  CU_ASSERT(slabsToScrub > 0);
  for (; slabsToScrub > 0; slabsToScrub--) {
    releaseSlabLatch(waitForAnySlabToLatch(totalSlabs));
  }

  // Suspend and resume the depot again so that we know the scrubber has
  // finished with the last slab
  performSuccessfulDepotAction(VDO_ADMIN_STATE_SUSPENDING);
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RESUMING);
  CU_ASSERT_FALSE(checkInRecovery());
  releaseAllSlabLatches(totalSlabs);
}

/**
 * Test that during the recovery, if a clean slab's reference count load is
 * deferred, its slab journal needs to be flushed before making a decision on
 * whether it needs to be scrubbed or not. Otherwise, if there are decRefs
 * added to the slab journal, and if the slab journal block is not written out,
 * that slab may not be scrubbed.
 **/
static void testSlabJournalFlush(void)
{
  initializeRecoveryModeT1(DEFAULT_MAPPABLE);
  // Fill the VDO and then restart to ensure slabs will loaded from the layer.
  block_count_t totalFreeBlocks = getPhysicalBlocksFree();
  writeAndVerifyData(1, 1, totalFreeBlocks, 0, totalFreeBlocks);

  // Flush out the all slab journals. This ensures the scrubbing order later.
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RECOVERING);
  restartVDO(false);

  block_count_t expectedFreeBlocks = 0;
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), expectedFreeBlocks);
  crashVDO();

  // vdo_slab 0 is scrubbed before coming online and slab 1 is the first slab
  // scrubbed during recovery mode.
  slabToLatch = 1;
  setupSlabLoadingLatch(slabToLatch);
  startVDO(VDO_DIRTY);
  waitForSlabLatch(slabToLatch);
  // The VDO should be in recovery mode after load finished.
  CU_ASSERT_TRUE(checkInRecovery());

  // Zero out another block in slab 2, which has not been scrubbed. This adds
  // an in-memory slab journal entry.
  logical_block_number_t slab2 = 1 + (dataPerSlab * 2);
  discardData(slab2 + 1, 1, VDO_SUCCESS);
  expectedFreeBlocks++;
  releaseSlabLatch(slabToLatch);

  waitForRecoveryDone();
  restartVDO(false);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), expectedFreeBlocks);
}

/**
 * Repeatedly write an alternating pattern of data, ensuring that the vdo
 * can write at least once around all journals.
 **/
static void fillJournals(block_count_t dataOffset)
{
  // Determine slab journal and recovery journal sizes.
  struct recovery_journal *journal     = vdo->recovery_journal;
  struct slab_journal     *slabJournal = &vdo->depot->slabs[0]->journal;
  block_count_t entriesToFillRecoveryJournal
    = journal->entries_per_block * journal->size;
  block_count_t entriesToFillAllSlabJournals
    = slabJournal->entries_per_block * slabJournal->size * totalSlabs;

  block_count_t recoveryJournalEntriesWritten = 0;
  block_count_t slabJournalEntriesWritten     = 0;
  block_count_t totalFreeBlocks               = getPhysicalBlocksFree();
  block_count_t half                          = totalFreeBlocks / 2;
  CU_ASSERT_TRUE(half > 0);
  while((recoveryJournalEntriesWritten < entriesToFillRecoveryJournal)
        || (slabJournalEntriesWritten < entriesToFillAllSlabJournals)) {
    writeData(0, dataOffset, half, VDO_SUCCESS);
    discardData(0, half, VDO_SUCCESS);
    writeData(0, dataOffset + half, half, VDO_SUCCESS);
    discardData(0, half, VDO_SUCCESS);
    recoveryJournalEntriesWritten += half * 8;
    slabJournalEntriesWritten     += half * 4;
  }
}

/**
 * Test that a VDO is fully functional even if some decrefs are added during
 * recovery mode.
 **/
static void testPostRecoveryMode(void)
{
  // Use a lot fewer mappable blocks so there are fewer (but at least 3) slabs.
  block_count_t totalFreeBlocks = 64;
  initializeRecoveryModeT1(totalFreeBlocks);

  // Write in slab-sized chunks so that we know which slab each LBN is in.
  writeBlocksSlabwise(dataPerSlab, 1, totalFreeBlocks, VDO_SUCCESS);

  crashVDO();

  // vdo_slab 0 is full of block map, slab 1 is scrubbed before coming online,
  // and slab 2 is the first slab scrubbed during recovery mode.
  slabToLatch = 2;
  startAndWaitForVDOInRecovery(false, VDO_DIRTY);
  // There should be precisely slabCount - 2 slabs on the scrubber.
  struct slab_scrubber *scrubber = &vdo->depot->allocators[0].scrubber;
  CU_ASSERT_EQUAL(totalSlabs - 2, READ_ONCE(scrubber->slab_count));

  // Launch a trim for everything for the slab which is scrubbing.
  IORequest *request = launchTrim(slabToLatch * dataPerSlab, dataPerSlab);

  for (slab_count_t i = 2; i < totalSlabs; i++) {
    if (i != slabToLatch) {
      discardData(i * dataPerSlab, dataPerSlab, VDO_SUCCESS);
    }
  }

  // Since all the other trims have finished, the entries for the scrubbing
  // slab must be queued in the slab journal.
  releaseAllSlabLatches(totalSlabs);
  awaitAndFreeSuccessfulRequest(vdo_forget(request));

  fillJournals(totalFreeBlocks + 1);
}

/**
 * Test that a VDO is fully functional after a read-only rebuild.
 **/
static void testPostReadOnlyRebuild(void)
{
  // Use a lot fewer mappable blocks so there are fewer (but at least 3) slabs.
  block_count_t totalFreeBlocks = 64;
  initializeRecoveryModeT1(totalFreeBlocks);

  writeData(0, 1, totalFreeBlocks, VDO_SUCCESS);

  rebuildReadOnlyVDO();
  verifyData(0, 1, totalFreeBlocks);
  discardData(0, totalFreeBlocks, VDO_SUCCESS);
  verifyZeros(0, totalFreeBlocks);

  // Make sure we can restart.
  restartVDO(false);

  fillJournals(totalFreeBlocks + 1);
}

/**
 * Test that we recompute logical blocks used correctly.
 **/
static void testAccounting(void)
{
  block_count_t totalFreeBlocks = 64;
  initializeRecoveryModeT1(totalFreeBlocks);

  writeData(0, 1, totalFreeBlocks, VDO_SUCCESS);
  performTrim(0, totalFreeBlocks / 2);
  for (logical_block_number_t lbn = totalFreeBlocks / 2; lbn < totalFreeBlocks; lbn++) {
    writeData(lbn, 0, 1, VDO_SUCCESS);
  }
  block_count_t allocated = getBlocksAllocated();

  crashVDO();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  verifyLogicalBlockUsed(totalFreeBlocks / 2);
  CU_ASSERT_EQUAL(getBlocksAllocated(), allocated);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Write during recovery",                    testRecoveryModeNoCompress },
  { "Wait for free space in unrecovered slabs", testFreeSpaceWait          },
  { "Free space wait doesn't hang on error",    testSlabScrubbingErrorHang },
  { "Requeue unrecovered slab",                 testRequeueUnrecoveredSlab },
  { "Suspend and resume while scrubbing", testSuspendAndResumeWhileScrubbing },
  { "vdo_slab journal flush on clean slabs",    testSlabJournalFlush       },
  { "Compress during recovery",                 testRecoveryCompress       },
  { "Fully operable after recovery",            testPostRecoveryMode       },
  { "Fully operable after read-only rebuild",   testPostReadOnlyRebuild    },
  { "Logical block accounting",                 testAccounting             },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "VDO recovery mode tests (RecoveryMode_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = tearDownRecoveryModeTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
