/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "admin-state.h"
#include "slab-depot.h"
#include "types.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "asyncVIO.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct vdo_slab       *slab;
static struct vdo_completion *slabJournalWrite;
static struct vdo_completion *refCountsWrite;
static bool                   blocked;
static bool                   draining;
static bool                   writeComplete;
static thread_id_t            physicalZoneThread;

/**
 * Initialize the test.
 **/
static void initialize(void)
{
  TestParameters parameters = {
    // Make sure there is only one physical zone
    .physicalThreadCount = 1,
    .slabCount           = 1,
    .slabSize            = 16,
    .dataFormatter       = fillWithOffsetPlusOne,
  };

  initializeVDOTest(&parameters);

  // Make sure the first tree is allocated down to the first leaf.
  writeData(0, 0, 1, VDO_SUCCESS);

  // Restart the VDO so that the pages are all written and the rest of the test
  // won't block if we trap writes.
  restartVDO(false);

  /*
   * Set the number of journal entries per block to 1 so that we only need to
   * write 1 block in order to get the journal to write. Set the flushing
   * threshold to 1 so that a reference count write will be triggered.
   */
  slab                                 = vdo->depot->slabs[0];
  slab->journal.entries_per_block      = 1;
  slab->journal.full_entries_per_block = 1;
  slab->journal.flushing_threshold     = 1;
  physicalZoneThread                   = vdo->thread_config.physical_threads[0];
}

/**
 * An action to check the state of a slab before running the saved callback
 * from the released write.
 *
 * Implements VDOAction.
 **/
static void checkSlabState(struct vdo_completion *completion)
{
  CU_ASSERT(vdo_is_state_draining(&slab->state));
  runSavedCallback(completion);
  signalState(&writeComplete);
}

/**
 * Trap one slab journal write and one RefCounts write (in their endio
 * callbacks).
 *
 * Implements CompletionHook.
 **/
static bool trapSlabWrites(struct vdo_completion *completion)
{
  if (!onBIOThread()
      || !isMetadataWrite(completion)
      || !vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  if (pbnFromVIO(vio) >= slab->journal_origin) {
    if (slabJournalWrite != NULL) {
      return true;
    }

    slabJournalWrite = completion;
  } else if (refCountsWrite != NULL) {
    return true;
  } else {
    refCountsWrite = completion;
  }

  wrapVIOCallback(vio, checkSlabState);
  if ((slabJournalWrite != NULL) && (refCountsWrite != NULL)) {
    clearCompletionEnqueueHooks();
    signalState(&blocked);
  }

  return false;
}

/**
 * Check whether the slab is draining.
 *
 * Implements FinishHook.
 **/
static void checkDraining(void)
{
  if (vdo_get_callback_thread_id() != physicalZoneThread) {
    return;
  }

  if (vdo_is_state_draining(&slab->state)) {
    signalState(&draining);
  }
}

/**
 * Test that the slab does not prematurely decide it has drained due to an
 * outstanding write.
 *
 * @param drainType     The type of drain to perform (suspend or save)
 * @param journalFirst  If <code>true</code>, release the journal write first,
 *                      otherwise release the RefCounts write first.
 **/
static void
testDrainWithBlockedWrite(const struct admin_state_code *drainType,
                          bool                           journalFirst)
{
  // Prepare to trap slab writes
  clearState(&blocked);
  slabJournalWrite = NULL;
  refCountsWrite   = NULL;
  setCompletionEnqueueHook(trapSlabWrites);

  // Write two blocks so that we trigger slab journal and reference count
  // writes.
  writeData(1, 1, 2, VDO_SUCCESS);
  waitForStateAndClear(&blocked);

  // Start draining
  clearState(&draining);
  setCallbackFinishedHook(checkDraining);
  struct vdo_completion *completion = launchSlabAction(slab, drainType);
  waitForState(&draining);

  struct vdo_completion *toRelease;
  if (journalFirst) {
    toRelease = UDS_FORGET(slabJournalWrite);
  } else {
    toRelease = UDS_FORGET(refCountsWrite);
    /*
     * The reference count block will have been redirtied by the second block
     * we wrote while it was trapped so it will get written again due to the
     * drain. If we are releasing the reference block write first, we want to
     * trap the second reference block write so that we can wait for it to be
     * done before releasing the journal write.
     */
    setCompletionEnqueueHook(trapSlabWrites);
  }

  /*
   * Release a write. If the slabs don't have the analogous problem to
   * [VDO-4800], this will not result in an early notification that the drain
   * is complete once we start draining below.
   */
  clearState(&writeComplete);
  reallyEnqueueCompletion(toRelease);
  waitForStateAndClear(&writeComplete);

  if (!journalFirst) {
    waitForState(&blocked);
    reallyEnqueueCompletion(refCountsWrite);
    waitForState(&writeComplete);
  }

  // Now release the other write. If we have fixed the bug, the slab will still
  // be suspending.
  reallyEnqueueCompletion(journalFirst ? refCountsWrite : slabJournalWrite);

  // Wait for the drain to complete
  awaitCompletion(completion);
  UDS_FREE(completion);

  // Resume the slab so that teardown succeeds.
  performSuccessfulSlabAction(slab, VDO_ADMIN_STATE_RESUMING);
}

/**
 * Test suspend with an outstanding slab journal write.
 **/
static void testSuspendJournalFirst(void) {
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SUSPENDING, true);
}

/**
 * Test save with an outstanding slab journal write.
 **/
static void testSaveJournalFirst(void) {
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SAVING, true);
}

/**
 * Test suspend with an outstanding RefCounts write.
 **/
static void testSuspendRefCountsFirst(void) {
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SUSPENDING, false);
}

/**
 * Test save with an outstanding RefCounts write.
 **/
static void testSaveRefCountsFirst(void) {
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SAVING, false);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test slab suspend journal drains first",   testSuspendJournalFirst,   },
  { "test slab save journal drains first",      testSaveJournalFirst,      },
  { "test slab suspend RefCounts drains first", testSuspendRefCountsFirst, },
  { "test slab save RefCounts drains first",    testSaveRefCountsFirst,    },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "test slab drain [VDO-4800]",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
