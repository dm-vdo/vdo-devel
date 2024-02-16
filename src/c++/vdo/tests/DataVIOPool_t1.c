/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/jiffies.h>

#include "logger.h"
#include "memory-alloc.h"
#include "thread-utils.h"

#include "types.h"

#include "asyncVIO.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  DATA_VIO_COUNT = 4,
  REQUEST_COUNT  = 13,
};

static const bool DISCARD = true;

// REQUEST_TYPES[lbn] says whether the request we launch for that LBN is a
// discard or a write.
static bool REQUEST_TYPES[REQUEST_COUNT] = {
  !DISCARD,
  !DISCARD,
  !DISCARD,
  !DISCARD,
  DISCARD,
  DISCARD,
  DISCARD,
  !DISCARD,
  !DISCARD,
  DISCARD,
  !DISCARD,
  DISCARD,
  DISCARD,
};

static logical_block_number_t launchOrder[REQUEST_COUNT] = {
  0, 1, 2, 3, 4, 5, 6, 9, 7, 8, 10, 11, 12,
};

static struct data_vio *blocked[REQUEST_COUNT + DATA_VIO_COUNT];
static uint8_t          blockedCount;
static uint8_t          nextLBNExpected;
static struct thread   *threads[REQUEST_COUNT];
static uint8_t          targetBlockedThreadCount;

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 64,
  };

  // Drastically reduce the data_vio_count so we can consume them all easily.
  data_vio_count = DATA_VIO_COUNT;
  blockedCount   = 0;
  initializeVDOTest(&parameters);
}

/**********************************************************************/
static bool blockDataVIOLocked(void *context)
{
  struct data_vio *dataVIO = context;
  CU_ASSERT_EQUAL(dataVIO->logical.lbn, nextLBNExpected++);
  blocked[dataVIO->logical.lbn] = dataVIO;
  blockedCount++;
  return true;
}

/**
 * Block any data_vio which is just launching, and assert that the launches
 * occur in the order we expect.
 **/
static bool blockAllLaunches(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_LAUNCH)) {
    return true;
  }

  runLocked(blockDataVIOLocked, as_data_vio(completion));
  return false;
}

/**********************************************************************/
static bool waitForBlockedCount(void *context)
{
  return (blockedCount == *((uint8_t *) context));
}

/**********************************************************************/
static void launchRequestOnThread(void *arg)
{
  logical_block_number_t lbn = *((logical_block_number_t *) arg);
  if (REQUEST_TYPES[lbn]) {
    discardData(lbn, 1, VDO_SUCCESS);
  } else {
    zeroData(lbn, 1, VDO_SUCCESS);
  }
}

/**********************************************************************/
static void launchRequest(logical_block_number_t lbn)
{
  char name[16];
  sprintf(name, "thread %" PRIu64, lbn);
  VDO_ASSERT_SUCCESS(uds_create_thread(launchRequestOnThread,
                                       &lbn,
                                       name,
                                       &threads[lbn]));
  targetBlockedThreadCount++;
  waitForCondition(checkBlockedThreadCount, &targetBlockedThreadCount);
}

/**********************************************************************/
static void releaseBlockedDataVIO(logical_block_number_t lbn)
{
  struct data_vio *data_vio = uds_forget(blocked[lbn]);
  CU_ASSERT_PTR_NOT_NULL(data_vio);
  reallyEnqueueVIO(&data_vio->vio);
}

/**********************************************************************/
static void joinThreadsUpTo(uint8_t limit)
{
  for (uint8_t i = 0; i < limit; i++) {
    if (threads[i] != NULL) {
      uds_join_threads(uds_forget(threads[i]));
    }
  }
}

/**
 * Test that the data vio pool correctly blocks threads when there are no
 * resources available, and then hands out those resources and wakes the
 * threads in the expected order.
 **/
static void testDataVIOPool(void)
{
  nextLBNExpected = REQUEST_COUNT;
  targetBlockedThreadCount = 0;
  setCompletionEnqueueHook(blockAllLaunches);

  // Launch a write to consume all the data_vios.
  IORequest *request = launchIndexedWrite(REQUEST_COUNT, 4, REQUEST_COUNT);
  waitForCondition(waitForBlockedCount, &data_vio_count);
  nextLBNExpected = 0;

  // Launch each of the remaining requests, each on its own thread.
  for (uint8_t i = 0; i < REQUEST_COUNT; i++) {
    launchRequest(launchOrder[i]);
  }

  // Release the 4 blocked data_vios (lbns 15-18).
  blockedCount = 0;
  for (uint8_t i = 0; i < data_vio_count; i++) {
    releaseBlockedDataVIO(i + REQUEST_COUNT);
  }

  awaitAndFreeRequest(uds_forget(request));

  // The 4 writes to lbns 0-3 should have been launched and blocked.
  waitForCondition(waitForBlockedCount, &data_vio_count);

  // Release the 4 blocked data_vios (lbns 0-3).
  blockedCount = 0;
  logical_block_number_t lbn;
  for (lbn = 0; lbn < DATA_VIO_COUNT; lbn++) {
    releaseBlockedDataVIO(lbn);
  }

  /*
   * The 3 discards for lbns 4-6 plus the write to lbn 7 should have been
   * launched and blocked. Even though the discard to lbn 9 was submitted before
   * the write to lbn 7, that discard can't get a permit, so the write should
   * go ahead of it.
   */
  waitForCondition(waitForBlockedCount, &data_vio_count);
  joinThreadsUpTo(lbn);

  // Release the blocked write (lbn 7), which should allow the write to lbn 8
  // to proceed. The blocked discard (lbn 9) still won't get a permit.
  blockedCount--;
  releaseBlockedDataVIO(7);
  waitForCondition(waitForBlockedCount, &data_vio_count);

  /*
   * Release all blocked data_vios (lbns 4 - 6 and 8). The bios for lbns 9-12
   * should all get launched. The blocked discard (lbn 9) should get launched
   * first now that it finally has a permit.
   */
  blockedCount = 0;
  for (; lbn < 9; lbn++) {
    if (lbn != 7) {
      releaseBlockedDataVIO(lbn);
    }
  }

  waitForCondition(waitForBlockedCount, &data_vio_count);
  joinThreadsUpTo(lbn);

  // Release all blocked data_vios (lbns 9 - 12) and everything should
  // complete.
  blockedCount = 0;
  for (; lbn < REQUEST_COUNT; lbn++) {
    releaseBlockedDataVIO(lbn);
  }

  joinThreadsUpTo(lbn);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test data vio pool contention", testDataVIOPool },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "data vio pool tests (DataVIOPool_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
