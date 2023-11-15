/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "flush.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static logical_block_number_t  lbnToBlock;
static bool                    flushState;
static struct bio              FLUSH_DONE;
static struct bio             *flushes[2];
static int                     flushCount;
static block_count_t           ackCount;
static block_count_t           targetAckCount;
static IORequest              *request;
static IORequest              *blocked;

/**
 * Setup VDO, then create a VDOFlush.
 **/
static void createVDOAndFlush(void)
{
  const TestParameters parameters = {
    .mappableBlocks     = 64,
    .journalBlocks      = 8,
    .enableCompression  = true,
  };
  initializeVDOTest(&parameters);

  lbnToBlock = 0;
  flushState = false;
  flushCount = 0;
  flushes[0] = flushes[1] = NULL;
}

/**
 * Implements LockedMethod.
 **/
static bool countAcknowledgmentsLocked(void *context __attribute__((unused)))
{
  ackCount++;
  return true;
}

/**
 * Count acknowledgments by counting vios enqueued on the bio ack queue.
 *
 * Implements CompletionHook
 **/
static bool countAcknowledgmentsHook(struct vdo_completion *completion)
{
  if (completion->callback_thread_id == vdo->thread_config.bio_ack_thread) {
    runLocked(countAcknowledgmentsLocked, NULL);
  }

  return true;
}

/**
 * Implements WaitCondition.
 **/
static bool checkAckCount(void *context)
{
  return (ackCount >= *((block_count_t *) context));
}

/**
 * We want to block a data_vio once we know that all the data_vios we've
 * launched have all gotten into the same flush generation. Because we only
 * have one logical zone, the data_vios will be added to the generation in
 * order, and shortly thereafter, will go on to acknowledge. Therefore, once
 * all of the data_vios have acknowledged, blocking the last of them will set
 * up the conditions we desire.
 *
 * Implements BlockCondition.
 **/
static bool
shouldBlockVIO(struct vdo_completion *completion,
               void                  *context __attribute__((unused)))
{
  return (isDataVIO(completion)
          && logicalIs(completion, lbnToBlock)
          && checkCondition(checkAckCount, &targetAckCount));
}

/**
 * Note that a flush has completed.
 *
 * Implements LockedMethod.
 *
 * @param context  The bio which is finishing
 **/
static bool recordFlushDoneLocked(void *context)
{
  for (int i = 0; i < flushCount; i++) {
    if (flushes[i] == context) {
      flushes[i] = &FLUSH_DONE;
      return true;
    }
  }

  return false;
}

/**
 * A bio endio function to record flush completions.
 *
 * Implements bio_end_io_t
 **/
static void recordFlushDone(struct bio *bio)
{
  runLocked(recordFlushDoneLocked, bio);
  uds_free(bio);
}

/**
 * Increment the flush count and return the previous value
 *
 * <p>Implements LockedMethod
 *
 * @param context  A pointer to hold the previous flush count
 **/
static bool incrementFlushCount(void *context)
{
  *((int *) context) = flushCount++;
  return false;
}

/**
 * Signal that a flush has started.
 *
 * <p>Implements vdo_action_fn
 **/
static void flushStartedCallback(struct vdo_completion *completion)
{
  int index;
  runLocked(incrementFlushCount, &index);

  // Record this flush as having been started so that later we can wait for it
  // to be finished.
  struct vdo_flush *flush = container_of(completion,
                                         struct vdo_flush,
                                         completion);
  flushes[index] = bio_list_peek(&flush->bios);
  runSavedCallback(completion);
  signalState(&flushState);
}

/**
 * If a completion is a new flush, wrap it.
 *
 * <p>Implements CompletionHook
 **/
static bool wrapFlush(struct vdo_completion *completion)
{
  // The initial launch, which we want to wrap, is from the test thread
  if ((completion->type == VDO_FLUSH_COMPLETION)
      && (vdo_get_callback_thread_id() == VDO_INVALID_THREAD_ID)) {
    wrapCompletionCallback(completion, flushStartedCallback);
    removeCompletionEnqueueHook(wrapFlush);
  }

  return true;
}

/**
 * Launch a flush and wait until it has started.
 **/
static void launchFlush(void)
{
  flushState = false;
  addCompletionEnqueueHook(wrapFlush);
  vdo_launch_flush(vdo, createFlushBIO(recordFlushDone));
  waitForStateAndClear(&flushState);
}

/**
 * Check whether a flush is done.
 *
 * <p>Implements WaitCondition
 *
 * @param context  A pointer to the index of the flush to check
 **/
static bool checkFlushDone(void *context)
{
  return (flushes[*((int *) context)] == &FLUSH_DONE);
}

/**
 * Check that a flush is not done.
 *
 * <p>Implements LockedMethod.
 *
 * @param context  A pointer to the index of the flush to check
 **/
static void assertFlushNotDone(void *context)
{
  CU_ASSERT_FALSE(checkFlushDone(context));
}

/**
 * Set up the first precondition by launching 5 writes, ensuring that they
 * all get into the same flush generation, and then block the last of them
 * so that a flush can not complete immediately. Then launch the flush.
 **/
static void launchFirstWritesAndFlush(void)
{
  ackCount       = 0;
  lbnToBlock     = 4;
  targetAckCount = 5;
  addCompletionEnqueueHook(countAcknowledgmentsHook);
  addBlockVIOCompletionEnqueueHook(shouldBlockVIO, true);
  request        = launchIndexedWrite(0, 4, 0);
  blocked        = launchIndexedWrite(lbnToBlock, 1, lbnToBlock);
  waitForBlockedVIO();
  launchFlush();
}

/**
 * Test the flush_vdo() function called by the kernel against blocked data
 * writes.
 **/
static void testDataVIOFlush(void)
{
  launchFirstWritesAndFlush();

  // Confirm everything except latched VIO is done.
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  int index = 0;
  assertFlushNotDone(&index);
  releaseBlockedVIO();
  awaitAndFreeSuccessfulRequest(uds_forget(blocked));
  waitForCondition(checkFlushDone, &index);
}

/**
 * Test VIO-interleaved flush() function calls.
 **/
static void testTwoVIOFlushes(void)
{
  launchFirstWritesAndFlush();
  IORequest *request2 = launchIndexedWrite(5, 5, 5);

  // We need this check to ensure that all the vios in the second batch
  // have gotten into the flush generation for the second flush.
  block_count_t ackTarget = 10;
  waitForCondition(checkAckCount, &ackTarget);

  // Make sure VIOs from the first set don't get into the second flush.
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  // Issue the second flush.
  launchFlush();

  // Finish the later write VIOs.
  awaitAndFreeSuccessfulRequest(uds_forget(request2));

  // Confirm neither flush is gone from VDO (still holding a first-flush VIO).
  for (int i = 0; i < 2; i++) {
    assertFlushNotDone(&i);
  }

  // Release the latched data-write VIO.
  releaseBlockedVIO();
  awaitAndFreeSuccessfulRequest(uds_forget(blocked));
  for (int i = 0; i < 2; i++) {
    waitForCondition(checkFlushDone, &i);
  }
}

/**********************************************************************/
static CU_TestInfo flushTests[] = {
  { "flush completes after VIOs",     testDataVIOFlush      },
  { "two flushes complete - VIOs",    testTwoVIOFlushes     },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo flushSuite = {
  .name                     = "Flush tests (Flush_t1)",
  .initializerWithArguments = NULL,
  .initializer              = createVDOAndFlush,
  .cleaner                  = tearDownVDOTest,
  .tests                    = flushTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &flushSuite;
}
