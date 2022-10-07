/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "wait-queue.h"

#include "albtest.h"
#include "vdoAsserts.h"

typedef struct waiterTracker {
  unsigned int   count;          // number of waiters and check
  struct waiter *waiters;
  unsigned int  *tracks;
  unsigned int   seq;
} WaiterTracker;

/**********************************************************************/
static void trackWaitersCalled(struct waiter *waiter, void *context)
{
  WaiterTracker *tracker = context;

  CU_ASSERT_TRUE(waiter >= tracker->waiters);
  CU_ASSERT_TRUE(waiter < &tracker->waiters[tracker->count]);
  tracker->tracks[waiter - tracker->waiters] = ++tracker->seq;
}

/**
 * Check that the tracker checks as expected.
 *
 * @param tracker       The WaiterTracker
 * @param expected      A string of "T", "F", ascii digits, or other chars.
 *                        For each entry in tracker, check if the corresponding
 *                        tracks field is either true (has been called) or
 *                        false (has not been called) or if it was called
 *                        in the specified order. Must not be longer than
 *                        the tracker's count field. Unrecognized characers
 *                        are skipped.
 **/
static void checkTracker(const WaiterTracker *tracker, const char *expected)
{
  unsigned int i = 0;
  for (const char *e = expected; *e != '\0'; ++e, ++i) {
    CU_ASSERT_TRUE(i < tracker->count);
    if (*e == 'T') {
      CU_ASSERT_TRUE(tracker->tracks[i]);
    } else if (*e == 'F') {
      CU_ASSERT_FALSE(tracker->tracks[i]);
    } else if ((*e >= '0') && (*e <= '9')) {
      CU_ASSERT_EQUAL(tracker->tracks[i], *e - '0');
    }
  }
}

/**********************************************************************/
static void basicTest(void)
{
  struct wait_queue queue;
  memset(&queue, 0, sizeof(queue));

  struct waiter waiters[5];
  memset(waiters, 0, sizeof(waiters));

  CU_ASSERT_FALSE(has_waiters(&queue));
  CU_ASSERT_EQUAL(0, count_waiters(&queue));

  enqueue_waiter(&queue, &waiters[0]);
  CU_ASSERT_TRUE(has_waiters(&queue));
  CU_ASSERT_EQUAL(1, count_waiters(&queue));

  enqueue_waiter(&queue, &waiters[2]);
  CU_ASSERT_EQUAL(2, count_waiters(&queue));

  enqueue_waiter(&queue, &waiters[3]);
  CU_ASSERT_EQUAL(3, count_waiters(&queue));

  unsigned int tracks[ARRAY_SIZE(waiters)];
  memset(&tracks, 0, sizeof(tracks));

  WaiterTracker tracker = {
    .count   = ARRAY_SIZE(waiters),
    .waiters = waiters,
    .tracks  = tracks,
    .seq     = 0,
  };

  CU_ASSERT_TRUE(notify_next_waiter(&queue, trackWaitersCalled, &tracker));

  checkTracker(&tracker, "TFFFF");

  CU_ASSERT_TRUE(has_waiters(&queue));
  CU_ASSERT_EQUAL(2, count_waiters(&queue));
  notify_all_waiters(&queue, trackWaitersCalled, &tracker);

  checkTracker(&tracker, "10230");

  CU_ASSERT_FALSE(has_waiters(&queue));
  CU_ASSERT_EQUAL(0, count_waiters(&queue));
  memset(&tracks, 0, sizeof(tracks));
  notify_all_waiters(&queue, trackWaitersCalled, &tracker);

  checkTracker(&tracker, "00000");
  CU_ASSERT_FALSE(notify_next_waiter(&queue, trackWaitersCalled, &tracker));

  struct wait_queue queue2;
  memset(&queue2, 0, sizeof(queue2));

  // transfer empty->empty
  transfer_all_waiters(&queue, &queue2);
  CU_ASSERT_EQUAL(0, count_waiters(&queue));
  CU_ASSERT_EQUAL(0, count_waiters(&queue2));

  // transfer single->empty
  enqueue_waiter(&queue, &waiters[0]);
  transfer_all_waiters(&queue, &queue2);
  CU_ASSERT_EQUAL(0, count_waiters(&queue));
  CU_ASSERT_EQUAL(1, count_waiters(&queue2));

  // transfer double->single
  enqueue_waiter(&queue, &waiters[1]);
  enqueue_waiter(&queue, &waiters[2]);
  transfer_all_waiters(&queue, &queue2);
  CU_ASSERT_EQUAL(0, count_waiters(&queue));
  CU_ASSERT_EQUAL(3, count_waiters(&queue2));

  // transfer empty->triple
  transfer_all_waiters(&queue, &queue2);
  CU_ASSERT_EQUAL(0, count_waiters(&queue));
  CU_ASSERT_EQUAL(3, count_waiters(&queue2));

  // transfer triple->empty
  transfer_all_waiters(&queue2, &queue);
  CU_ASSERT_EQUAL(3, count_waiters(&queue));
  CU_ASSERT_EQUAL(0, count_waiters(&queue2));
}

/**********************************************************************/
static void iterationTest(void)
{
  struct wait_queue queue;
  memset(&queue, 0, sizeof(queue));

  struct waiter waiters[5];
  memset(waiters, 0, sizeof(waiters));

  struct waiter *order[4];

  struct waiter **wp = order;

  *wp++ = &waiters[3];
  *wp++ = &waiters[2];
  *wp++ = &waiters[0];
  *wp++ = &waiters[4];

  CU_ASSERT_EQUAL(wp - order, ARRAY_SIZE(order));

  for (wp = order; wp < &order[ARRAY_SIZE(order)]; ++wp) {
    enqueue_waiter(&queue, *wp);
  }

  wp = order;
  for (const struct waiter *w = get_first_waiter(&queue);
       w != NULL;
       w = get_next_waiter(&queue, w))
  {
    CU_ASSERT_TRUE(wp < &order[ARRAY_SIZE(order)]);
    CU_ASSERT_PTR_EQUAL(w, *wp);
    ++wp;
  }

  CU_ASSERT_PTR_EQUAL(wp, &order[ARRAY_SIZE(order)]);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "basic wait queue",    basicTest     },
  { "iterate wait queues", iterationTest },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "WaitQueue_t1",
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
