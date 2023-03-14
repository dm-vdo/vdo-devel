/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "latchedCloseUtils.h"

#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static CloseInfo             closeInfo;
static struct vdo_completion closeCompletion;
static int                   expectedResult = VDO_SUCCESS;
static bool                  closeDone      = false;
static bool                  expectClosure  = false;

/**
 * Signal that the close has completed.
 *
 * <p>Implements VDOAction.
 **/
static void
signalCloseComplete(struct vdo_completion *completion __attribute__((unused)))
{
  signalState(&closeDone);
}

/**
 * Run the object closer and finish when it's done with the synchronous
 * part of its work.
 *
 * <p>Implements VDOAction.
 **/
static void runCloseObject(struct vdo_completion *completion)
{
  vdo_initialize_completion(&closeCompletion, vdo, VDO_TEST_COMPLETION);
  vdo_prepare_completion(&closeCompletion,
                         signalCloseComplete,
                         signalCloseComplete,
                         completion->callback_thread_id,
                         NULL);
  (*closeInfo.launcher)(closeInfo.closeContext, &closeCompletion);
  vdo_finish_completion(completion);
}

/**
 * Check the closedness of the object and assert that it is equal to the
 * expected closedness.
 *
 * <p>Implements VDOAction.
 **/
static void assertCloseStatus(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL((*closeInfo.checker)(closeInfo.closeContext), expectClosure);
  vdo_finish_completion(completion);
}

/**********************************************************************/
void runLatchedClose(CloseInfo info, int result)
{
  closeInfo      = info;
  expectedResult = result;
  closeDone      = false;
  expectClosure  = false;

  performSuccessfulActionOnThread(runCloseObject, info.threadID);
  performSuccessfulActionOnThread(assertCloseStatus, info.threadID);
  (*info.releaser)(info.releaseContext);
  waitForState(&closeDone);
  expectClosure = true;
  performSuccessfulActionOnThread(assertCloseStatus, info.threadID);
}
