/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "completion.h"
#include "recovery-journal.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static bool                     notificationTrapped;
static struct recovery_journal *journal;
static sequence_number_t        blockMapReapHead;
static sequence_number_t        slabJournalReapHead;
static struct vdo_completion   *notification;

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .logicalBlocks       = 1024,
    .journalBlocks       = 16,
    .logicalThreadCount  = 1,
    .physicalThreadCount = 1,
    .hashZoneThreadCount = 1,
  };
  initializeVDOTest(&parameters);

  // Save some time by making the journal blocks smaller.
  journal = vdo->recovery_journal;
  journal->entries_per_block = 8;

  blockMapReapHead    = 1;
  slabJournalReapHead = 1;
}

/**
 * Trap the lock counter notification.
 *
 * Implements CompletionHook.
 **/
static bool trapNotification(struct vdo_completion *completion)
{
  if (completion->type != VDO_LOCK_COUNTER_COMPLETION) {
    return true;
  }

  clearCompletionEnqueueHooks();
  notification = completion;
  signalState(&notificationTrapped);
  return false;
}

/**
 * Test whether or not the journal has reaped, and record the current reap
 * heads. This method must be called from the journal thread.
 *
 * @param shouldHaveReaped  <code>true</code> if we expect the journal to have
 *                          reaped
 **/
static void checkReapHeads(bool shouldHaveReaped)
{
  CU_ASSERT_EQUAL(shouldHaveReaped,
                  (blockMapReapHead != journal->block_map_reap_head));
  CU_ASSERT_EQUAL(shouldHaveReaped,
                  (slabJournalReapHead != journal->slab_journal_reap_head));
  blockMapReapHead = journal->block_map_reap_head;
  slabJournalReapHead = journal->slab_journal_reap_head;
}

/**
 * Verify that the journal is quiescing, then release the trapped notification
 * and verify that the journal is quiescent. Also check that the journal
 * didn't reap.
 *
 * Implements VDOAction.
 **/
static void releaseNotification(struct vdo_completion *completion)
{
  struct vdo_completion *notificationCompletion = notification;
  notification = NULL;
  CU_ASSERT(vdo_is_state_quiescing(&journal->state));
  vdo_run_completion_callback(notificationCompletion);
  CU_ASSERT(vdo_is_state_quiescent(&journal->state));
  checkReapHeads(false);
  vdo_complete_completion(completion);
}

/**
 * Blow up on a lock counter notification.
 *
 * Implements CompletionHook.
 **/
static bool failOnNotification(struct vdo_completion *completion)
{
  CU_ASSERT(completion->type != VDO_LOCK_COUNTER_COMPLETION);
  return true;
}

/**
 * Action to check that the journal has reaped.
 *
 * Implements VDOAction.
 **/
static void assertReaped(struct vdo_completion *completion)
{
  checkReapHeads(true);
  vdo_complete_completion(completion);
}

/**
 * Test that the lock counter is correctly suspended and resumed.
 **/
static void testLockCounterSuspend(void)
{
  clearState(&notificationTrapped);
  setCompletionEnqueueHook(trapNotification);

  // Write two full journal blocks of data.
  VDO_ASSERT_SUCCESS(performIndexedWrite(0, journal->entries_per_block * 2,
                                         1));

  // Suspend and resume the journal to ensure that it is quiescent.
  performSuccessfulRecoveryJournalAction(VDO_ADMIN_STATE_SUSPENDING);
  performSuccessfulRecoveryJournalAction(VDO_ADMIN_STATE_RESUMING);

  // Save the block map which should trigger a notification.
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_SAVING);
  waitForState(&notificationTrapped);

  // Initiate a drain of the journal which should not complete due to the
  // pending notification.
  struct vdo_completion *drain
    = launchRecoveryJournalAction(journal, VDO_ADMIN_STATE_SUSPENDING);

  // Release the trapped notification checking that the journal isn't yet
  // quiescent, but then is when the notification completes.
  thread_id_t journalThread = notification->callback_thread_id;
  performSuccessfulActionOnThread(releaseNotification, journalThread);
  awaitCompletion(drain);
  UDS_FREE(drain);

  // Save the slab depot, blowing up if it sends a notification.
  setCompletionEnqueueHook(failOnNotification);
  performSuccessfulDepotAction(VDO_ADMIN_STATE_SAVING);
  clearCompletionEnqueueHooks();

  // Resume everything
  performSuccessfulRecoveryJournalAction(VDO_ADMIN_STATE_RESUMING);
  performSuccessfulActionOnThread(assertReaped, journalThread);

  performSuccessfulDepotAction(VDO_ADMIN_STATE_RESUMING);
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RESUMING);

  // Write two more journal blocks worth of data.
  VDO_ASSERT_SUCCESS(performIndexedWrite(0, journal->entries_per_block * 2,
                                         1));
  // Set up to trap the notification again which we expect to come from
  // the next round of saving and resuming.
  clearState(&notificationTrapped);
  setCompletionEnqueueHook(trapNotification);

  // Save and resume the block map and slab depot which should trigger reaping.
  performSuccessfulDepotAction(VDO_ADMIN_STATE_SAVING);
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_SAVING);
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RESUMING);
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RESUMING);

  waitForState(&notificationTrapped);
  reallyEnqueueCompletion(UDS_FORGET(notification));
  // Now that we know the notification is enqueued on the journal thread,
  // it is no longer racy to enqueue the reap check (VDO-5381).
  performSuccessfulActionOnThread(assertReaped, journalThread);
}


/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test lock counter suspend and resume", testLockCounterSuspend  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Test recovery journal draining (RecoveryJournalDrain_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
