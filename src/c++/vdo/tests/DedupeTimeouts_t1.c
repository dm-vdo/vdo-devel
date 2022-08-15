/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "uds.h"

#include "dedupe.h"
#include "statistics.h"
#include "vdo.h"

#include "dedupeContext.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "testTimer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  TIMEOUT_COUNT = 8,
  TOTAL_COUNT   = 2 * TIMEOUT_COUNT,
};

static struct uds_request *blockedRequests[TOTAL_COUNT];
static vio_count_t         blockedCount;
static struct data_vio    *querying;
static bool                queryDone;

// The dedupe requests which will not be timed out, chosen (arbitrarily) to
// provide different sized groupings and gaps in the pending list.
static const uint8_t ALLOW_TO_DEDUPE[] = {
  1, 4, 5, 9, 10, 12, 14, 15,
};

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks    = 64,
    .dataFormatter     = fillWithOffsetPlusOne,
  };
  initializeVDOTest(&parameters);
}

/**********************************************************************/
static bool blockDedupeRequestLocked(void *context)
{
  blockedRequests[blockedCount++] = context;
  if (blockedCount == TOTAL_COUNT) {
    uds_chunk_operation_hook = NULL;
  }

  return true;
}

/**********************************************************************/
static int blockDedupeRequest(struct uds_request *request)
{
  if (request->type == UDS_UPDATE) {
    return UDS_SUCCESS;
  }

  runLocked(blockDedupeRequestLocked, request);
  return UDS_ERROR_CODE_LAST;
}

/**********************************************************************/
static bool allRequestsBlocked(void *context __attribute__((unused)))
{
  return (blockedCount == TOTAL_COUNT);
}

/**********************************************************************/
static bool signalQueryComplete(struct vdo_completion *completion)
{
  if (isDataVIO(completion) && (as_data_vio(completion) == querying)) {
    clearCompletionEnqueueHooks();
    signalState(&queryDone);
  }

  return true;
}

/**
 * Test that data_vios with dedupe timeouts eventually end up on the
 * compression path, and that interleaved data_vios which get processed will
 * deduplicate.
 **/
static void testDedupeTimeouts(void)
{
  vdo_set_dedupe_index_timeout_interval(1000);
  vdo_set_dedupe_index_min_timer_interval(2);

  // Write 16 blocks of unique data
  VDO_ASSERT_SUCCESS(performIndexedWrite(0, TOTAL_COUNT, 0));

  // Write duplicates, but block all their dedupe requests
  blockedCount = 0;
  uds_chunk_operation_hook = blockDedupeRequest;
  IORequest *request = launchIndexedWrite(TOTAL_COUNT, TOTAL_COUNT, 0);
  waitForCondition(allRequestsBlocked, NULL);

  queryDone = false;
  uint8_t completeIndex = 0;
  for (vio_count_t i = 0; i < TOTAL_COUNT; i++) {
    if (ALLOW_TO_DEDUPE[completeIndex] != i) {
      continue;
    }

    struct dedupe_context *context = container_of(blockedRequests[i],
                                                  struct dedupe_context,
                                                  request);
    fireTimers(context->submission_jiffies + 250);
    querying = context->requestor;
    setCompletionEnqueueHook(signalQueryComplete);
    VDO_ASSERT_SUCCESS(uds_start_chunk_operation(blockedRequests[i]));
    waitForStateAndClear(&queryDone);
    completeIndex++;
  }

  awaitAndFreeSuccessfulRequest(request);

  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.dedupe_advice_timeouts, TIMEOUT_COUNT);
  CU_ASSERT_EQUAL(stats.data_blocks_used, TOTAL_COUNT + TIMEOUT_COUNT);
  CU_ASSERT_EQUAL(stats.hash_lock.curr_dedupe_queries, TIMEOUT_COUNT);

  completeIndex = 0;
  for (vio_count_t i = 0; i < TOTAL_COUNT; i++) {
    if (ALLOW_TO_DEDUPE[completeIndex] == i) {
      completeIndex++;
      continue;
    }

    VDO_ASSERT_SUCCESS(uds_start_chunk_operation(blockedRequests[i]));
  }
}


/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test dedupe timeouts", testDedupeTimeouts },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "dedupe timeout tests (DedupeTimeouts_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
