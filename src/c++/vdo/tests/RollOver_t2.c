/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "data-vio.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static block_count_t viosWaitingForHashLock;

/**
 * Test-specific initialization.
 **/
static void initializeTest(void)
{
  const TestParameters parameters = {
    .mappableBlocks      = 2,
    .logicalBlocks       = 265,
    .logicalThreadCount  = 3, // Arbitrary (from VDO_t1)
    .physicalThreadCount = 2, // Arbitrary (from VDO_t1)
    .hashZoneThreadCount = 2, // Arbitrary (from VDO_t1)
  };
  initializeVDOTest(&parameters);

  viosWaitingForHashLock = 0;
}

/**********************************************************************/
static bool explodeOnVerification(struct bio *bio)
{
  struct vio *vio = bio->bi_private;

  if (lastAsyncOperationIs(&vio->completion, VIO_ASYNC_OP_VERIFY_DUPLICATION)) {
    CU_FAIL("attempted to verify a block that rolls over");
  }

  return true;
}

/**
 * Release the blocked VIO when two VIOs are queued on its hash lock.
 *
 * <p>Implements VDOAction.
 **/
static void countHashLockWaiters(struct vdo_completion *completion)
{
  // Assertion will fire if the VIO got requeued in the callback, which
  // it shouldn't have if it's waiting on the hash lock.
  runSavedCallbackAssertNoRequeue(completion);
  if (++viosWaitingForHashLock != 2) {
    return;
  }

  reallyEnqueueVIO(getBlockedVIO());
  broadcast();
}

/**
 * Check if a VIO we care about is just questing for acquiring a hash lock.
 *
 * Implements CompletionHook.
 */
static bool wrapIfAcquiringHashLock(struct vdo_completion *completion)
{
  /*
   * This depends on lockHashInZone() being a callback that is always enqueued
   * because it is always triggered on a thread other than the appropriate
   * hash zone thread for obtaining the lock.
   */
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_ACQUIRE_VDO_HASH_LOCK)
      && (viosWaitingForHashLock < 2)) {
    wrapCompletionCallback(completion, countHashLockWaiters);
  }

  return true;
}

/**
 * Check whether the number of VIOs waiting on a hash lock is the desired
 * value.
 *
 * Implements WaitCondition.
 **/
static bool checkVIOsWaitingForHashLock(void *context __attribute__((unused)))
{
  return (viosWaitingForHashLock == 2);
}

/**
 * Implements CompletionHook.
 **/
static bool blockFirstVIO(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_LOCK_DUPLICATE_PBN)) {
    return true;
  }

  clearCompletionEnqueueHooks();
  blockVIO(as_vio(completion));
  return false;
}

/**
 * Test roll-over when full.
 **/
static void testFill(void)
{
  // Fill all but one data block
  block_count_t          mappable = populateBlockMapTree();
  // The loop below is going to use LBNs 0-253 and this needs to not be in that
  // range.
  logical_block_number_t lbn      = 254;
  writeAndVerifyData(lbn, 1, mappable - 1, 1, mappable - 1);

  // Write duplicate data until we roll over (the first iteration will fill up
  // the physical space).
  for (unsigned int iteration = 0; iteration < 254; iteration++) {
    writeAndVerifyData(iteration, mappable, 1, 0, mappable);

    // Verify that the space is full.
    writeData(lbn, mappable + 1, 1, VDO_NO_SPACE);
  }

  // Check that HashLock will roll over without even verifying the duplicate
  // when the PBN lock is acquired with all increments consumed.
  setBIOSubmitHook(explodeOnVerification);

  // Set up to trap the first VIO we write while it holds a hash lock.
  setCompletionEnqueueHook(blockFirstVIO);

  lbn += mappable;
  IORequest *request = launchIndexedWrite(lbn, 1, mappable);
  waitForBlockedVIO();

  // Launch two more writes of the same data.
  setCompletionEnqueueHook(wrapIfAcquiringHashLock);
  IORequest *request2 = launchIndexedWrite(++lbn, 1, mappable);
  writeData(++lbn, mappable, 1, VDO_NO_SPACE);
  waitForCondition(checkVIOsWaitingForHashLock, NULL);
  clearCompletionEnqueueHooks();

  CU_ASSERT_EQUAL(awaitAndFreeRequest(UDS_FORGET(request)), VDO_NO_SPACE);
  CU_ASSERT_EQUAL(awaitAndFreeRequest(UDS_FORGET(request2)), VDO_NO_SPACE);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "fill an entire VDO",  testFill  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "Roll over of full VDO (RollOver_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
