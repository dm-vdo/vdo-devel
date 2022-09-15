// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "uds-threads.h"
#include "testPrototypes.h"
#ifndef __KERNEL__
#include "valgrind/valgrind.h"
#endif

/**********************************************************************/
static void testAttemptSemaphore(void)
{
  struct semaphore semaphore;
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&semaphore, 1));

  // Just make sure we've wrapped the function correctly and are returning the
  // correct values for success and failure.

  CU_ASSERT_TRUE(uds_attempt_semaphore(&semaphore, 0));   // 1 -> 0
  CU_ASSERT_FALSE(uds_attempt_semaphore(&semaphore, 0));  // 0: fail
  CU_ASSERT_FALSE(uds_attempt_semaphore(&semaphore, 0));  // 0: fail
  uds_release_semaphore(&semaphore);                      // 0 -> 1
  CU_ASSERT_TRUE(uds_attempt_semaphore(&semaphore, 0));   // 1 -> 0
  uds_release_semaphore(&semaphore);                      // 0 -> 1

  UDS_ASSERT_SUCCESS(uds_destroy_semaphore(&semaphore));
}

/**********************************************************************/
static void testSemaphoreTimeout(void)
{
  struct semaphore semaphore;
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&semaphore, 1));

  // Check timeout variant when a permit is available.
  CU_ASSERT_TRUE(uds_attempt_semaphore(&semaphore, 0));  // 1 -> 0

  // Check that we return false when timing out with no permit.
  CU_ASSERT_FALSE(uds_attempt_semaphore(&semaphore, 0)); // 0: fail

  // Check that the timeout is actually being used by looking at how often the
  // attempt call has an elapsed time not close to the timeout.
  enum { ITERATIONS = 200 };
#ifdef __KERNEL__
  unsigned long timeout_usec = 5000;
  // uds_attempt_semaphore uses down_timeout which takes its timeout in
  // jiffies.
  unsigned long jiffy_usec = jiffies_to_usecs(1UL);
  if ((2 * jiffy_usec) > timeout_usec) {
    timeout_usec = 2 * jiffy_usec;
    albPrint("  raising timeout to %lu usec due to large jiffy granularity",
             timeout_usec);
  }
  ktime_t timeout = us_to_ktime(timeout_usec);
#else
  ktime_t timeout = us_to_ktime(500);
#endif
  int tooShort = 0;
  int tooLong  = 0;
  int i;
  for (i = 0; i < ITERATIONS; i++) {
    ktime_t startTimer = current_time_ns(CLOCK_MONOTONIC);
    CU_ASSERT_FALSE(uds_attempt_semaphore(&semaphore, timeout));
    ktime_t elapsed
      = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTimer);
    // If the timeout is too small, overhead hides the timeout, so make
    // sure we don't take too long, either.
    if (elapsed < timeout) {
      albPrint("elapsed=%ld  timeout=%ld", (long) elapsed, (long) timeout);
      tooShort += 1;
    } else if (elapsed > 2 * timeout) {
      tooLong += 1;
    }
  }

  int failures = tooShort + tooLong;
  if (failures >= (ITERATIONS / 10)) {
    albPrint("timeout failures: %d, tooShort=%d, tooLong=%d",
             failures, tooShort, tooLong);
  }
  UDS_ASSERT_SUCCESS(uds_destroy_semaphore(&semaphore));

#ifndef __KERNEL__
  // Running under valgrind throws off all the timing, so skip the upcoming
  // assertions.
  if (RUNNING_ON_VALGRIND) {
    return;
  }
#endif

  // Allow the timeout check to fail 10% of the time, which will hopefully be
  // enough slack to tolerate scheduler effects without losing test
  // discrimination.
  CU_ASSERT_TRUE(failures < (ITERATIONS / 10));
}

/**
 * Parameters and globals for testBarriers().
 **/
enum {
  BARRIER_THREAD_COUNT      = 4,
  BARRIER_THREAD_ITERATIONS = 500
};

static struct barrier  barrier1;
static struct barrier  barrier2;

/**
 * Worker thread driver function for testBarrier().
 **/
static void barrierWorker(void *arg __attribute__((unused)))
{
  int i;
  for (i = 0; i < BARRIER_THREAD_ITERATIONS; i++) {
    UDS_ASSERT_SUCCESS(uds_enter_barrier(&barrier1));
    UDS_ASSERT_SUCCESS(uds_enter_barrier(&barrier2));
  }
}

/**
 * Check that the barrier functions appear to work correctly. This is not an
 * exhaustive test of pthread barriers, but merely a simple test that the
 * wrappers are plugged in to the pthread calls and don't have anything wired
 * backwards.
 **/
static void testBarriers(void)
{
  UDS_ASSERT_SUCCESS(uds_initialize_barrier(&barrier1, BARRIER_THREAD_COUNT));
  UDS_ASSERT_SUCCESS(uds_initialize_barrier(&barrier2, BARRIER_THREAD_COUNT));

  // Fork and join worker threads to exercise the barriers.
  struct thread *threads[BARRIER_THREAD_COUNT];
  int i;
  for (i = 0; i < BARRIER_THREAD_COUNT; i++) {
    UDS_ASSERT_SUCCESS(uds_create_thread(barrierWorker, NULL, "barrierWorker",
                                         &threads[i]));
  }
  for (i = 0; i < BARRIER_THREAD_COUNT; i++) {
    UDS_ASSERT_SUCCESS(uds_join_threads(threads[i]));
  }

  UDS_ASSERT_SUCCESS(uds_destroy_barrier(&barrier1));
  UDS_ASSERT_SUCCESS(uds_destroy_barrier(&barrier2));
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"attemptSemaphore",     testAttemptSemaphore},
  {"semaphore timeout",    testSemaphoreTimeout},
  {"barriers",             testBarriers},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "Threads_t1",
  .tests = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
