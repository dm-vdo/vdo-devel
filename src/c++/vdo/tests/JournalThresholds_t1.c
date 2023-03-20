/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "encodings.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "types.h"
#include "vdo.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "recoveryModeUtils.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct recovery_journal  sampledJournal;
static block_count_t            slabSummaryWriteCount;
static logical_block_number_t  *slabLBNs;
static logical_block_number_t  *slabLBNs2;
static bool                     reaping;

// Ensure no dedupe by writing distinct data blocks in sequence.
static physical_block_number_t  nextDataBlock;

/**
 * Get a slab journal from a specific slab.
 *
 * @param  slabNumber  the slab number of the slab journal
 **/
static struct slab_journal *getVDOSlabJournal(slab_count_t slabNumber)
{
  return vdo->depot->slabs[slabNumber]->journal;
}

/**
 * Hook to record up to two LBNs which are mapped to each slab.
 *
 * Implements CompletionHook.
 **/
static bool recordLBN(struct vdo_completion *completion)
{
  if (!isDataWrite(completion)) {
    return true;
  }

  struct data_vio        *dataVIO = as_data_vio(completion);
  logical_block_number_t  lbn     = dataVIO->logical.lbn;
  slab_count_t slabNumber
    = vdo_get_slab(vdo->depot, dataVIO->new_mapped.pbn)->slab_number;
  if (slabLBNs[slabNumber] == 0) {
    slabLBNs[slabNumber] = lbn;
  } else if (slabLBNs2[slabNumber] == 0) {
    slabLBNs2[slabNumber] = lbn;
  }

  return true;
}

/**
 * Action to interrogate the journal.
 **/
static void sampleJournal(struct vdo_completion *completion)
{
  sampledJournal = *(vdo->recovery_journal);
  vdo_finish_completion(completion);
}

/**
 * Interrogate the journal until it is not reaping.
 **/
static void interrogateJournalUntilNotReaping(void)
{
  performSuccessfulAction(sampleJournal);
  while (sampledJournal.reaping) {
    performSuccessfulAction(sampleJournal);
  }
}

/**********************************************************************/
static void setSlabJournalEntriesPerBlock(journal_entry_count_t entriesPerBlock)
{
  for (slab_count_t slab = 0; slab < vdo->depot->slab_count; slab++) {
    struct slab_journal *slabJournal    = vdo->depot->slabs[slab]->journal;
    slabJournal->entries_per_block      = entriesPerBlock;
    slabJournal->full_entries_per_block = entriesPerBlock;
    slabJournal->blocking_threshold     = slabJournal->scrubbing_threshold;
  }
}

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks      = 252,
    .journalBlocks       = 16,
    .slabJournalBlocks   = 16,
    .physicalThreadCount = 1,
  };
  initializeRecoveryModeTest(&parameters);

  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(vdo->depot->slab_count, logical_block_number_t,
                                  __func__, &slabLBNs));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(vdo->depot->slab_count, logical_block_number_t,
                                  __func__, &slabLBNs2));

  setCompletionEnqueueHook(recordLBN);
  nextDataBlock = 1;
  nextDataBlock += fillPhysicalSpace(nextDataBlock, 1);
  clearCompletionEnqueueHooks();
  restartVDO(false);

  // Slab journals are flushed.
  for (slab_count_t slab = 1; slab < vdo->depot->slab_count; slab++) {
    struct slab_journal *slabJournal = getVDOSlabJournal(slab);
    CU_ASSERT_EQUAL(slabJournal->last_summarized, 2);
  }
}

/**********************************************************************/
static void tearDownTest(void)
{
  UDS_FREE(slabLBNs2);
  UDS_FREE(slabLBNs);
  tearDownRecoveryModeTest();
}

/**
 * This fills the recovery journal and also fills a specific slab journal to
 * just before its flushing threshold by issuing a write pattern of trim,
 * overwrites, and a write.
 *
 * Note that the write pattern only works correctly when the VDO is full and
 * there is no deduplication.
 *
 * @param  slabNumber  the index of the slab to use
 * @param  numEntries  the number of entries to add to the recovery journal.
 *                     The slab journal for the specified slab gets
 *                     (numEntries - 2) entries since zero block writes
 *                     do not add a slab journal entry.
 **/
static void issueOverwriteAtSlab(slab_count_t slabNumber, size_t numEntries)
{
  // This function can only add an even number of entries.
  CU_ASSERT_TRUE((numEntries & 0x1) == 0x0);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);

  // Trim a block in the slab to create room for an overwrite.
  logical_block_number_t trimBlock = slabLBNs[slabNumber];
  discardData(trimBlock, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);

  // Issue overwrites to fill up the slab journal.
  logical_block_number_t overwriteBlock = slabLBNs2[slabNumber];
  for (size_t remaining = numEntries - 1; remaining > 1; --remaining) {
    writeData(overwriteBlock, nextDataBlock++, 1, VDO_SUCCESS);
    CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);
  }

  // Write data to fill the empty block, which also creates one more entry.
  writeData(trimBlock, nextDataBlock++, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);
}

/**
 * This adds entries to a slab journal while filling the recovery journal.
 *
 * @param  slabNumber  the slab to use
 * @param  numEntries  the number of entries to add to the slab journal
 **/
static void addEntriesToSlabJournal(slab_count_t slabNumber, size_t numEntries)
{
  issueOverwriteAtSlab(slabNumber, numEntries + 2);
}

/**
 * Implements VDOAction.
 **/
static void countSummaryWrites(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  slabSummaryWriteCount++;
  broadcast();
}

/**
 * Wrap a slab summary write.
 *
 * Implements CompletionHook.
 **/
static bool wrapSlabSummaryWrite(struct vdo_completion *completion)
{
  if (onBIOThread()
      && isMetadataWrite(completion)
      && vioTypeIs(completion, VIO_TYPE_SLAB_SUMMARY)) {
    wrapCompletionCallback(completion, countSummaryWrites);
  }

  return true;
}

/**
 * Implements WaitCondition.
 **/
static bool checkSlabSummaryWriteCount(void *context)
{
  return (slabSummaryWriteCount == *((block_count_t *) context));
}

/**
 * Wait for slab summary block writes.
 *
 * @param writeCount  the number block writes to wait for
 **/
static void waitForSlabSummaryBlockWrites(block_count_t writeCount)
{
  waitForCondition(checkSlabSummaryWriteCount, &writeCount);
}

/**********************************************************************/
static bool failOnSlabJournalReap(struct vdo_completion *completion)
{
  struct recovery_journal *journal = vdo->recovery_journal;
  if ((completion == &journal->flush_vio->completion)
      && (vdo_get_callback_thread_id() == vdo->thread_config->journal_thread)) {
    CU_ASSERT_EQUAL(journal->slab_journal_head, journal->slab_journal_reap_head);
  }

  return true;
}

/**********************************************************************/
static bool notifySlabJournalReap(struct vdo_completion *completion)
{
  struct recovery_journal *journal = vdo->recovery_journal;
  if ((completion == &journal->flush_vio->completion)
      && (vdo_get_callback_thread_id() == vdo->thread_config->journal_thread)
      && (journal->slab_journal_head < journal->slab_journal_reap_head)) {
    signalState(&reaping);
  }

  return true;
}

/**
 * When the recovery journal threshold is reached, the oldest slab journal
 * tails are written out.
 **/
static void testRecoveryJournalThreshold(void)
{
  vdo->recovery_journal->entries_per_block = 32;
  setSlabJournalEntriesPerBlock(32);

  // Check that the journal is completely reaped with respect to slabs.
  performSuccessfulAction(sampleJournal);
  CU_ASSERT_PTR_NULL(sampledJournal.active_block);
  CU_ASSERT_EQUAL(sampledJournal.tail, sampledJournal.slab_journal_head);

  addCompletionEnqueueHook(failOnSlabJournalReap);
  sequence_number_t expectedHead = sampledJournal.slab_journal_head;
  const size_t ONE_BLOCK = sampledJournal.entries_per_block;
  // Make slab journal 0 and 1 hold locks on the 1st recovery journal block.
  issueOverwriteAtSlab(0, ONE_BLOCK / 2);
  issueOverwriteAtSlab(1, ONE_BLOCK / 2);
  // Make slab journal 1 and 2 hold locks on the 2nd recovery journal block.
  issueOverwriteAtSlab(1, ONE_BLOCK / 2);
  issueOverwriteAtSlab(2, ONE_BLOCK / 2);

  // Issue writes on slab 3 to fill the recovery journal to just before its
  // threshold.
  size_t remainingBlocks = sampledJournal.slab_journal_commit_threshold - 2;
  issueOverwriteAtSlab(3, (ONE_BLOCK * remainingBlocks));

  // Verify that the recovery journal has not been reaped and the threshold
  // has not been crossed.
  performSuccessfulAction(sampleJournal);
  CU_ASSERT_EQUAL(sampledJournal.slab_journal_head, expectedHead++);
  CU_ASSERT_EQUAL((sampledJournal.tail - sampledJournal.slab_journal_head),
                  sampledJournal.slab_journal_commit_threshold);

  // Verify the rest of the slab journals are empty and not committed.
  for (slab_count_t slab = 4; slab < vdo->depot->slab_count; slab++) {
    CU_ASSERT_EQUAL(getVDOSlabJournal(slab)->last_summarized, 2);
    CU_ASSERT_EQUAL(getVDOSlabJournal(slab)->tail_header.entry_count, 0);
  }

  // Issue another write at slab 3. Recovery journal will hit the threshold.
  reaping = false;
  setCompletionEnqueueHook(notifySlabJournalReap);
  slabSummaryWriteCount = 0;
  addCompletionEnqueueHook(wrapSlabSummaryWrite);
  discardData(slabLBNs[3], 1, VDO_SUCCESS);
  writeData(slabLBNs[3], nextDataBlock++, 1, VDO_SUCCESS);

  waitForSlabSummaryBlockWrites(2);
  waitForState(&reaping);

  // Verify that slab journals 0 and 1 are committed and one recovery journal
  // block is reaped.
  interrogateJournalUntilNotReaping();
  CU_ASSERT_EQUAL(getVDOSlabJournal(0)->last_summarized, 3);
  CU_ASSERT_EQUAL(getVDOSlabJournal(1)->last_summarized, 3);
  CU_ASSERT_EQUAL(sampledJournal.slab_journal_head, expectedHead);
  // Verify that slab journal 2 tail is not committed.
  CU_ASSERT_EQUAL(getVDOSlabJournal(2)->last_summarized, 2);
}

/**
 * Check for a recovery_journal block write.
 *
 * Implements BlockCondition.
 **/
static bool
isRecoveryJournalBlockWrite(struct vdo_completion *completion,
                            void *context __attribute__((unused)))
{
  return (vioTypeIs(completion, VIO_TYPE_RECOVERY_JOURNAL)
          && isMetadataWrite(completion));
}

/**
 * Check for recovery mode.
 *
 * Implements BlockCondition.
 **/
static bool
checkRecoveryMode(struct vdo_completion *completion,
                  void                  *context __attribute__((unused)))
{
  return vdo_in_recovery_mode(completion->vdo);
}

/**
 * Test that the slab is scrubbed if it does not have enough slab journal
 * space.
 **/
static void testScrubSlabDuringRebuild(void)
{
  setSlabJournalEntriesPerBlock(256);
  slab_count_t slabNumber = 1;
  setupSlabScrubbingLatch(slabNumber);

  // Fill slab journal 1 to its blocking/scrubbing threshold.
  struct slab_journal  *slabJournal = getVDOSlabJournal(slabNumber);
  block_count_t alreadyWritten      = (slabJournal->tail - slabJournal->head);
  const size_t  ONE_BLOCK           = slabJournal->entries_per_block;
  block_count_t blockingThreshold   = slabJournal->blocking_threshold;
  block_count_t blocksUntilBlocking = blockingThreshold - alreadyWritten;
  addEntriesToSlabJournal(slabNumber, (ONE_BLOCK * blocksUntilBlocking / 2) - 2);

  // By now, we should have attempted to write several reference blocks.
  waitForSlabLatch(slabNumber);

  // We should not take a snapshot until the slab summary reflects the
  // current slab journal tail.
  slabSummaryWriteCount = 0;
  setCompletionEnqueueHook(wrapSlabSummaryWrite);

  // Do one more overwrite to cause a slab journal and slab summary write.
  issueOverwriteAtSlab(slabNumber, 2);
  waitForSlabSummaryBlockWrites(1);
  clearCompletionEnqueueHooks();

  // Launch a zero block write which will be blocked in the slab journal.
  setBlockVIOCompletionEnqueueHook(isRecoveryJournalBlockWrite, true);
  logical_block_number_t trimBlock = slabLBNs2[slabNumber];
  IORequest *trim = launchIndexedWrite(trimBlock, 1, 0);

  // Wait until the recovery journal updates with the increment for this trim.
  struct vio *blockedVIO = getBlockedVIO();

  struct packed_journal_sector *lastSector
    = vdo->recovery_journal->active_block->sector;
  journal_entry_count_t lastEntry = lastSector->entry_count - 1;
  struct recovery_journal_entry entry
    = vdo_unpack_recovery_journal_entry(&lastSector->entries[lastEntry]);
  CU_ASSERT_EQUAL(trimBlock, entry.slot.slot);
  CU_ASSERT_EQUAL(0, entry.mapping.pbn);

  // Release the journal block's first write, and catch its second (which
  // contains the decrement which will block in the slab journal).
  setBlockVIOCompletionEnqueueHook(isRecoveryJournalBlockWrite, true);
  reallyEnqueueVIO(blockedVIO);
  blockedVIO = getBlockedVIO();

  // Take a snapshot of the current VDO on-disk content.
  PhysicalLayer *slabJournalFull = cloneRAMLayer(getSynchronousLayer());

  reallyEnqueueVIO(blockedVIO);
  releaseSlabLatch(slabNumber);

  awaitAndFreeSuccessfulRequest(UDS_FORGET(trim));
  stopVDO();

  // Replace the ram layer content with snapshot content.
  copyRAMLayer(getSynchronousLayer(), slabJournalFull);
  slabJournalFull->destroy(&slabJournalFull);

  // Restart the VDO.
  setBlockBIO(checkRecoveryMode, true);
  startVDO(VDO_DIRTY);
  blockedVIO  = getBlockedVIO();
  slabJournal = getVDOSlabJournal(slabNumber);
  CU_ASSERT_EQUAL(slabJournal->head, slabJournal->tail);
  CU_ASSERT_EQUAL(slabJournal->tail_header.entry_count, 0);

  reallyEnqueueBIO(blockedVIO->bio);
  waitForRecoveryDone();
}

/**********************************************************************/
static CU_TestInfo vdoTests[] = {
  { "recovery journal threshold", testRecoveryJournalThreshold },
  { "scrub slab during rebuild",  testScrubSlabDuringRebuild   },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name  = "journal thresholds (JournalThresholds_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
