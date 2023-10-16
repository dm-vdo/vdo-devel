// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "funnel-requestqueue.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "uds-threads.h"

static unsigned int        count;
static struct uds_request *found;

/**********************************************************************/
static void singleWorker(struct uds_request *req)
{
  count++;
  found = req;
}

/**********************************************************************/
static void basicTest(void)
{
  struct uds_request *requests;
  struct uds_request_queue *queue;

  count = 0;
  found = NULL;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(2, struct uds_request, __func__, &requests));
  requests[0].unbatched = true;
  requests[1].unbatched = true;

  UDS_ASSERT_SUCCESS(uds_make_request_queue("single", &singleWorker, &queue));
  CU_ASSERT_PTR_NOT_NULL(queue);

  uds_request_queue_enqueue(queue, &requests[0]);
  uds_request_queue_enqueue(queue, &requests[1]);
  uds_request_queue_finish(queue);

  CU_ASSERT_PTR_EQUAL(&requests[1], found);
  CU_ASSERT_EQUAL(2, count);

  UDS_FREE(requests);
}

/**********************************************************************/
static struct uds_request_queue *priorityTestQueue          = NULL;
static bool                      processedRetryRequest      = false;
static bool                      enqueuedRetryRequest       = false;
static bool                      needNextRequestRetryStatus = false;
static bool                      nextRequestRetryStatus     = false;
static struct semaphore          requestSemaphore;

/**********************************************************************/
static void priorityTestWorker(struct uds_request *req)
{
  if (needNextRequestRetryStatus) {
    nextRequestRetryStatus = req->requeued;
    needNextRequestRetryStatus = false;
  }
  /*
   * The status field is used as a hack here.  If it's zero, we just
   * keep requeueing the requests to keep the worker thread busy.  If
   * it's nonzero, that's the signal to trigger the main part of the
   * test -- enqueueing a retry request and verifying that it's the
   * next one processed after we complete the current one.
   */
  if (req->status == 0) {
    if (req->requeued) {
      processedRetryRequest = true;
    }
    // Just keep requeueing this one unless we're wrapping up.
    if (!processedRetryRequest) {
      uds_request_queue_enqueue(priorityTestQueue, req);
    } else {
      // Let main thread know any time we let a request die.
      uds_release_semaphore(&requestSemaphore);
    }
  } else {
    // Now that we've got other stuff in the queue, add a retry/new pair.
    CU_ASSERT_FALSE(enqueuedRetryRequest);
    req->status = 0;
    req->requeued = true;
    uds_request_queue_enqueue(priorityTestQueue, req);
    enqueuedRetryRequest = true;
    needNextRequestRetryStatus = true;
  }
}

/**********************************************************************/
static void retryPriorityTest(void)
{
  struct uds_request *requests;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(3, struct uds_request, __func__, &requests));
  requests[0].unbatched = false;
  requests[1].unbatched = true;
  requests[2].unbatched = true;
  requests[2].status    = 1;
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&requestSemaphore, 0));

  UDS_ASSERT_SUCCESS(uds_make_request_queue("priority", &priorityTestWorker,
                                            &priorityTestQueue));
  CU_ASSERT_PTR_NOT_NULL(priorityTestQueue);

  processedRetryRequest = false;
  enqueuedRetryRequest  = false;

  uds_request_queue_enqueue(priorityTestQueue, &requests[0]);
  uds_request_queue_enqueue(priorityTestQueue, &requests[1]);

  // Let the worker thread run for a bit, then trigger the test.
  sleep_for(us_to_ktime(100));
  uds_request_queue_enqueue(priorityTestQueue, &requests[2]);

  // Wait for the requests to be processed.  This test normally runs in 2 to 5
  // milliseconds, so 1 second is a long timeout.  We use 100 seconds.
  ktime_t timeout = seconds_to_ktime(100);
  CU_ASSERT_TRUE(uds_attempt_semaphore(&requestSemaphore, timeout));
  CU_ASSERT_TRUE(uds_attempt_semaphore(&requestSemaphore, timeout));
  CU_ASSERT_TRUE(uds_attempt_semaphore(&requestSemaphore, timeout));
  uds_request_queue_finish(priorityTestQueue);

  CU_ASSERT_TRUE(enqueuedRetryRequest);
  CU_ASSERT_TRUE(processedRetryRequest);
  CU_ASSERT_TRUE(nextRequestRetryStatus);

  UDS_ASSERT_SUCCESS(uds_destroy_semaphore(&requestSemaphore));
  UDS_FREE(requests);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Basic",         basicTest    },
  { "RetryPriority", retryPriorityTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name    = "RequestQueue_t1",
  .cleaner = NULL,
  .tests   = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
