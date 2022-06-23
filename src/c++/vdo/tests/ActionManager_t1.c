/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "admin-state.h"
#include "block-map.h"
#include "completion.h"
#include "recovery-journal.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

bool                   suspendScheduled;
bool                   zoneActionTrapped;
struct vdo_completion *blocked;
sequence_number_t      increment;

/**
 * An action to advance the block map era.
 **/
static void advanceBlockMapEraAction(struct vdo_completion *completion)
{
  vdo_advance_block_map_era(vdo->block_map,
                            vdo->recovery_journal->tail + increment++);
  vdo_complete_completion(completion);
}

/**********************************************************************/
static void testSchedulerWhenQuiescent(void)
{
  increment = 5;
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_SUSPENDING);
  performSuccessfulActionOnThread(advanceBlockMapEraAction, 0);
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RESUMING);
}

/**
 * Trap the action manager completion on the first zone action.
 *
 * Implements CompletionHook.
 **/
static bool trapZoneAction(struct vdo_completion *completion)
{
  if (completion->type != VDO_ACTION_COMPLETION) {
    return true;
  }

  clearCompletionEnqueueHooks();
  blocked = completion;
  signalState(&zoneActionTrapped);
  return false;
}


/**
 * An action to advance the block map era.
 **/
static void suspendBlockMapAction(struct vdo_completion *completion)
{
  vdo_drain_block_map(vdo->block_map, VDO_ADMIN_STATE_SUSPENDING, completion);
  signalState(&suspendScheduled);
}

/**
 * Test that attempting to schedule multiple default actions does not prevent
 * the action manager from performing a suspend [VDO-5006].
 **/
static void testRepeatedDefaultAction(void)
{
  increment = 5;
  clearState(&zoneActionTrapped);
  setCompletionEnqueueHook(trapZoneAction);
  performSuccessfulActionOnThread(advanceBlockMapEraAction, 0);
  waitForState(&zoneActionTrapped);
  performSuccessfulActionOnThread(advanceBlockMapEraAction, 0);
  performSuccessfulActionOnThread(advanceBlockMapEraAction, 0);

  struct vdo_completion suspend;
  vdo_initialize_completion(&suspend, vdo, VDO_TEST_COMPLETION);
  suspend.callback_thread_id
    = vdo_get_logical_zone_thread(vdo->thread_config, 0);
  clearState(&suspendScheduled);
  launchAction(suspendBlockMapAction, &suspend);
  waitForState(&suspendScheduled);

  struct vdo_completion *toRelease = blocked;
  blocked                          = NULL;
  reallyEnqueueCompletion(toRelease);
  VDO_ASSERT_SUCCESS(awaitCompletion(&suspend));

  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RESUMING);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "manager doesn't loop when not normal",     testSchedulerWhenQuiescent },
  { "default action doesn't consume all slots", testRepeatedDefaultAction  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Action manager (ActionManager_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultVDOTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}

