// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * This is a performance and stress test of event count, a lock-free
 * equivalent of a condition variable.
 *
 * The test has two threads passing a "message" (a pointer to an integer) back
 * and forth in a very tight loop without any queueing or buffering. There's a
 * single global pointer variable used to exchange the messages. An event count
 * is used to allow the message sender to wait for a reply (the next incoming
 * message). The main driver thread forks an adder thread that waits for a
 * message (an integer x) which it sends back to the main thread as an
 * incremented reply (x + 1).
 *
 * The test is actually implemented three times with three different
 * mechanisms: an event count a mutex & condition variable, and just spinning
 * on the shared variable, waiting for it to change. This provides a context
 * for the performance of event count.
 **/

#include "albtest.h"
#include "assertions.h"
#include "event-count.h"
#include "memory-alloc.h"
#include "uds-threads.h"
#include "time-utils.h"

/** Shared variables for event count test */
static int * volatile ecMessage;
static struct event_count *eventCount;

/**********************************************************************/
static void ecAdder(void *arg __attribute__((unused)))
{
  int reply;
  // Wait for a non-NULL message - a NULL message means that the loop in
  // the main thread has not started yet
  while (ecMessage == NULL) {
    event_token_t token = event_count_prepare(eventCount);
    if (ecMessage == NULL) {
      CU_ASSERT_TRUE(event_count_wait(eventCount, token, NULL));
    } else {
      event_count_cancel(eventCount, token);
    }
  }
  for (;;) {
    // Wait for a message (an integer to increment) from the driver thread.
    while (ecMessage == &reply) {
      event_token_t token = event_count_prepare(eventCount);
      if (ecMessage == &reply) {
        CU_ASSERT_TRUE(event_count_wait(eventCount, token, NULL));
      } else {
        event_count_cancel(eventCount, token);
      }
    }
    // A NULL message is the signal to shut down.
    if (ecMessage == NULL) {
      break;
    }
    // Increment the value in the message and send it as the reply.
    reply = (*ecMessage + 1);
    ecMessage = &reply;
    event_count_broadcast(eventCount);
  }
}

/**********************************************************************/
static void testEventCount(int messageCount)
{
  albPrint("    EventCount starting %d iterations", messageCount);

  UDS_ASSERT_SUCCESS(make_event_count(&eventCount));

  struct thread *adderThread;
  UDS_ASSERT_SUCCESS(uds_create_thread(ecAdder, NULL, "eventCount",
                                       &adderThread));

  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);

  int x;
  for (x = 0; x < messageCount; ) {
    // send the loop variable as a message to the adder thread.
    ecMessage = &x;
    event_count_broadcast(eventCount);

    // Wait for the adder thread to send a reply (the incremented value).
    while (ecMessage == &x) {
      event_token_t token = event_count_prepare(eventCount);
      if (ecMessage == &x) {
        CU_ASSERT_TRUE(event_count_wait(eventCount, token, NULL));
      } else {
        event_count_cancel(eventCount, token);
      }
    }

    // increment the loop variable by assigning the reply value (x + 1).
    x = *ecMessage;
  }

  ktime_t ecTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);

  // Stop the adder thread by sending a NULL message.
  ecMessage = NULL;
  event_count_broadcast(eventCount);
  uds_join_threads(adderThread);

  char *ecTotal, *ecPer;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&ecTotal, ecTime, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&ecPer, ecTime, messageCount));
  albPrint("    event count %s, %s/increment", ecTotal, ecPer);
  UDS_FREE(ecTotal);
  UDS_FREE(ecPer);
  free_event_count(eventCount);
}

/** Shared variables for mutex test */
static struct mutex    mutex;
static struct cond_var cond;
static int     *mutexMessage;

/**********************************************************************/
static void mutexAdder(void *arg __attribute__((unused)))
{
  int reply;
  uds_lock_mutex(&mutex);
  for (;;) {
    // Wait for a message (an integer to increment) from the driver thread.
    while (mutexMessage == &reply) {
      uds_wait_cond(&cond, &mutex);
    }
    // A NULL message is the signal to shut down.
    if (mutexMessage == NULL) {
      break;
    }
    // Increment the value in the message and send it as the reply.
    reply = (*mutexMessage + 1);
    mutexMessage = &reply;
    uds_signal_cond(&cond);
  }
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
static void testMutex(int messageCount)
{
  albPrint("    mutex starting %d iterations", messageCount);

  UDS_ASSERT_SUCCESS(uds_init_mutex(&mutex));
  UDS_ASSERT_SUCCESS(uds_init_cond(&cond));

  uds_lock_mutex(&mutex);
  struct thread *adderThread;
  UDS_ASSERT_SUCCESS(uds_create_thread(mutexAdder, NULL, "mutex",
                                       &adderThread));

  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);

  int x;
  for (x = 0; x < messageCount; ) {
    // send the loop variable as a message to the adder thread.
    mutexMessage = &x;
    uds_signal_cond(&cond);

    // Wait for the adder thread to send a reply (the incremented value).
    while (mutexMessage == &x) {
      uds_wait_cond(&cond, &mutex);
    }

    // increment the loop variable by assigning the reply value (x + 1).
    x = *mutexMessage;
  }

  ktime_t mutexTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);

  // Stop the adder thread by sending a NULL message.
  mutexMessage = NULL;
  uds_signal_cond(&cond);
  uds_unlock_mutex(&mutex);
  uds_join_threads(adderThread);

  char *mutexTotal, *mutexPer;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&mutexTotal, mutexTime, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&mutexPer, mutexTime, messageCount));
  albPrint("    mutex %s, %s/increment", mutexTotal, mutexPer);
  UDS_FREE(mutexTotal);
  UDS_FREE(mutexPer);
  uds_destroy_cond(&cond);
  uds_destroy_mutex(&mutex);
}

/** Shared variables for spin loop test */
static int * volatile spinMessage;

/**********************************************************************/
static void spinAdder(void *arg __attribute__((unused)))
{
  int reply;
  // Wait for a non-NULL message - a NULL message means that the loop in
  // the main thread has not started yet
  while (spinMessage == NULL) {
    cond_resched();
  }
  for (;;) {
    // Wait for a message (an integer to increment) from the driver thread.
    while (spinMessage == &reply) {
      cond_resched();
    }
    // A NULL message is the signal to shut down.
    if (spinMessage == NULL) {
      break;
    }
    // Increment the value in the message and send it as the reply.
    reply = (*spinMessage + 1);
    spinMessage = &reply;
  }
}

/**********************************************************************/
static void testSpinLoop(int messageCount)
{
  albPrint("    spin loop starting %d iterations", messageCount);

  UDS_ASSERT_SUCCESS(make_event_count(&eventCount));

  struct thread *adderThread;
  UDS_ASSERT_SUCCESS(uds_create_thread(spinAdder, NULL, "spin", &adderThread));
  ktime_t startTime = current_time_ns(CLOCK_MONOTONIC);

  int x;
  for (x = 0; x < messageCount; ) {
    // send the loop variable as a message to the adder thread.
    spinMessage = &x;

    // Wait for the adder thread to send a reply (the incremented value).
    while (spinMessage == &x) {
      cond_resched();
    }

    // increment the loop variable by assigning the reply value (x + 1).
    x = *spinMessage;
  }

  ktime_t spinTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), startTime);

  // Stop the adder thread by sending a NULL message.
  spinMessage = NULL;
  uds_join_threads(adderThread);

  char *spinTotal, *spinPer;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&spinTotal, spinTime, 0));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&spinPer, spinTime, messageCount));
  albPrint("    spin loop %s, %s/increment", spinTotal, spinPer);
  UDS_FREE(spinTotal);
  UDS_FREE(spinPer);
  free_event_count(eventCount);
}

/**********************************************************************/
static void syncTest(void)
{
  enum {
    MESSAGE_COUNT = 10 * 1000 * 1000
  };

  // Mutex is significantly slower, so test it with fewer iterations.
  testMutex(MESSAGE_COUNT / 10);

  testEventCount(MESSAGE_COUNT);

  if (uds_get_num_cores() > 1) {
    testSpinLoop(MESSAGE_COUNT);
  } else {
    // Spin loop is extraordinarily slow on a single core.
    testSpinLoop(100);
  }
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "synchronization", syncTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "EventCount_p1",
  .tests = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
