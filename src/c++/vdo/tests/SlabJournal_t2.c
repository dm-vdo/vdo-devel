/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/list.h>

#include "memory-alloc.h"

#include "block-map.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct recovery_journal *journal;
static struct slab_journal     *slabJournal;
static bool                     slabJournalBlocked;
static bool                     recoveryJournalBlocked;
static bool                     slabJournalHasPassedBlocking;
static block_count_t            blocksWritten;
static thread_id_t              slabJournalThread;
static vdo_action              *wrapper;
/**
 * Test-specific initialization.
 **/
static void initializeSlabJournalT2(void)
{
  const TestParameters parameters = {
    // The slab size must be bigger than the number of entries which fit in the
    // slab journal.
    .slabSize             = 256,
    .logicalBlocks        = 512,
    .slabCount            = 1,
    .slabJournalBlocks    = 8,
    .journalBlocks        = 16,
    .dataFormatter        = fillWithOffsetPlusOne,
    .physicalThreadCount  = 1,
    .disableDeduplication = true,
  };
  initializeVDOTest(&parameters);
  populateBlockMapTree();
  blocksWritten = fillPhysicalSpace(0, 0);
  addSlabs(1);
  // Restart the VDO so all journals are effectively empty.
  restartVDO(false);

  // Cache the journal for slab 1 and reduce its size in order to speed up
  // this test.
  journal                    = vdo->recovery_journal;
  journal->entries_per_block = 16;
  journal->available_space   = (vdo_get_recovery_journal_length(journal->size)
                                * journal->entries_per_block);

  slabJournal                    = &vdo->depot->slabs[1]->journal;
  slabJournal->entries_per_block = vdo->recovery_journal->entries_per_block;

  slabJournalThread            = slabJournal->slab->allocator->thread_id;
  slabJournalBlocked           = false;
  recoveryJournalBlocked       = false;
  slabJournalHasPassedBlocking = false;
}

/**
 * A callback wrapper to check whether the slab journal is blocked.
 *
 * Implements VDOAction.
 **/
static void checkForSlabJournalBlocked(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  if (vdo_has_waiters(&slabJournal->entry_waiters)) {
    clearCompletionEnqueueHooks();
    signalState(&slabJournalBlocked);
  }
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfInPhysicalZone(struct vdo_completion *completion)
{
  if (isDataVIO(completion)
      && (completion->callback_thread_id == slabJournalThread)) {
    wrapCompletionCallback(completion, wrapper);
  }

  return true;
}

/**
 * A callback wrapper to signal when the recovery journal has blocked.
 *
 * Implements VDOAction.
 **/
static void checkForRecoveryJournalBlocked(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  if (vdo_has_waiters(&vdo->recovery_journal->entry_waiters)
      && (vdo->recovery_journal->available_space == 0)) {
    signalState(&recoveryJournalBlocked);
  }
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfInJournalZone(struct vdo_completion *completion)
{
  if (isDataVIO(completion)
      && (completion->callback_thread_id == journal->thread_id)) {
    wrapCompletionCallback(completion, checkForRecoveryJournalBlocked);
  }

  return true;
}

/**
 * Grab all of the VIO pool entries from the block allocator, then fill the
 * first slab journal block of the journal for slab 1. Finally, launch one
 * more write to slab 1 which will block waiting for the slab journal to
 * commit.
 *
 * @return The blocked write request
 **/
static IORequest *setUpBlockedJournal(void)
{
  reserveVIOsFromPool(slabJournal->slab->allocator,
                      BLOCK_ALLOCATOR_VIO_POOL_SIZE);

  // Fill the first slab journal block by writing new data which will go to
  // slab 1.
  writeData(blocksWritten, blocksWritten, slabJournal->entries_per_block,
            VDO_SUCCESS);
  blocksWritten += slabJournal->entries_per_block;

  // Write one more block to the same slab which will block since the
  // slab journal commit is waiting for a VIO pool entry.
  wrapper = checkForSlabJournalBlocked;
  setCompletionEnqueueHook(wrapIfInPhysicalZone);
  IORequest *request = launchIndexedWrite(blocksWritten, 1, blocksWritten);
  waitForState(&slabJournalBlocked);
  return request;
}

/**
 * Test trimming enough blocks to advance to the recovery journal threshold
 * while VIOs are blocked waiting to make slab journal entries.
 **/
static void testSlabJournalCommitDelay(void)
{
  IORequest *request = setUpBlockedJournal();

  struct recovery_journal *journal = vdo->recovery_journal;
  setCompletionEnqueueHook(wrapIfInJournalZone);
  IORequest *trim = launchTrim(blocksWritten + 1, journal->available_space + 1);
  waitForState(&recoveryJournalBlocked);
  returnVIOsToPool();

  // Everything should complete.
  awaitAndFreeSuccessfulRequest(uds_forget(request));
  awaitAndFreeSuccessfulRequest(uds_forget(trim));
}

/**
 * Check whether a VIO is about to write a reference count block.
 *
 * <p>Implements BlockCondition.
 **/
static bool
isRefCountsWrite(struct vdo_completion *completion,
                 void                  *context __attribute__((unused)))
{
  if (!is_vio(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  physical_block_number_t origin = slabJournal->slab->ref_counts_origin;
  physical_block_number_t bound
    = origin + vdo->depot->slab_config.reference_count_blocks;
  physical_block_number_t pbn = pbnFromVIO(vio);
  return ((bio_op(vio->bio) == REQ_OP_WRITE)
          && (pbn >= origin)
          && (pbn < bound));
}

/**
 * An action to request that slab journal 0 release its recovery journal lock.
 *
 * @param completion  The completion for this action
 **/
static void releaseRecoveryJournalLockAction(struct vdo_completion *completion)
{
  sequence_number_t lock = slabJournal->recovery_lock;
  CU_ASSERT_TRUE(release_recovery_journal_lock(slabJournal, lock));
  vdo_finish_completion(completion);
}

/**
 * A callback wrapper to check that the slab journal tail has advanced to an
 * appropriate point.
 *
 * Implements vdo_action.
 **/
static void checkSlabJournalTail(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  if (slabJournal->tail > slabJournal->blocking_threshold) {
    clearCompletionEnqueueHooks();
    signalState(&slabJournalHasPassedBlocking);
  }
}

/**********************************************************************/
static void assertSlabJournalClean(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(slabJournal->recovery_lock, 0);
  vdo_finish_completion(completion);
}

/**********************************************************************/
static void assertSlabJournalDirty(struct vdo_completion *completion)
{
  CU_ASSERT_NOT_EQUAL(slabJournal->recovery_lock, 0);
  vdo_finish_completion(completion);
}

/**
 * Test lock release request on a slab journal at the blocking threshold.
 **/
static void testLockReleaseRequestOnBlockedSlabJournal(void)
{
  // Block the first reference block write so that the journal can fill up.
  setBlockBIO(isRefCountsWrite, true);

  // Fill the slab journal writing data which was never written before to
  // avoid dedupe against previously written and trimmed data.
  block_count_t blockCount
    = (slabJournal->entries_per_block * slabJournal->blocking_threshold);
  writeData(blocksWritten, blocksWritten, blockCount, VDO_SUCCESS);
  blocksWritten += blockCount;

  // Write one more block.
  wrapper = checkForSlabJournalBlocked;
  setCompletionEnqueueHook(wrapIfInPhysicalZone);
  IORequest *request = launchIndexedWrite(blocksWritten, 1, blocksWritten);
  waitForState(&slabJournalBlocked);
  wrapper = checkSlabJournalTail;
  setCompletionEnqueueHook(wrapIfInPhysicalZone);

  // Ask the slab journal to release recovery journal locks (nothing happens,
  // because the lock in question is a per-entry lock held by the waiting VIO).
  performSuccessfulActionOnThread(assertSlabJournalClean, slabJournalThread);
  performSuccessfulActionOnThread(releaseRecoveryJournalLockAction,
                                  slabJournalThread);
  performSuccessfulActionOnThread(assertSlabJournalClean, slabJournalThread);

  // Release the blocked reference count write. The request should complete,
  // and the slab journal should commit.
  reallyEnqueueBIO(getBlockedVIO()->bio);
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  // Actually cause the tail block to be written --- letting the waiting VIO
  // make an entry, and thus making the journal dirty.
  performSuccessfulActionOnThread(assertSlabJournalDirty, slabJournalThread);
  performSuccessfulActionOnThread(releaseRecoveryJournalLockAction,
                                  slabJournalThread);
  performSuccessfulActionOnThread(assertSlabJournalClean, slabJournalThread);
  waitForState(&slabJournalHasPassedBlocking);
}

/**
 * Test that flushing a slab journal which is waiting to launch a tail block
 * commit does eventually flush.
 **/
static void testSlabJournalFlushDelay(void)
{
  IORequest *request = setUpBlockedJournal();

  // Flush the slab journal.
  struct vdo_completion *flushCompletion
    = launchSlabAction(slabJournal->slab, VDO_ADMIN_STATE_RECOVERING);

  // Return the VIO pool entries.
  returnVIOsToPool();

  // The request should complete.
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  // The flush should complete.
  VDO_ASSERT_SUCCESS(awaitCompletion(flushCompletion));
  uds_free(flushCompletion);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test delaying of partial block commits",    testSlabJournalCommitDelay },
  { "test recovery release request to blocked journal",
    testLockReleaseRequestOnBlockedSlabJournal },
  { "test delaying of slab journal flush",       testSlabJournalFlushDelay  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "SlabJournal_t2",
  .initializer              = initializeSlabJournalT2,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
