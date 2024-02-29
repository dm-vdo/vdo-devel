// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/*
 * This is a performance test that measures different ways of implementing
 * request queues in the kernel.  We simultaneously run 4 producer threads and
 * consumer thread, putting 5 million things on the queue.
 */

#include <linux/cache.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "albtest.h"
#include "assertions.h"
#include "event-count.h"
#include "funnel-queue.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "thread-utils.h"

/*
 * The alignment is because the real things we care about on request queues
 * will either have similar alignment or will be large enough to never share a
 * cache line with another request queue entry.
 */
typedef struct queueable {
  long              stream __aligned(L1_CACHE_BYTES);
  long              number;
  struct funnel_queue_entry funnel;
  struct list_head  list;
  struct llist_node llist;
} Queueable;

typedef struct queueableBatch {
  long                    count;
  long                    stream;
  long                    active;
  atomic_t                counter;
  struct completion       wait;
  struct list_head        list;
  struct wait_queue_head  wqhead;
  struct llist_head       llist;
  struct funnel_queue    *funnel;
  struct event_count     *event;
  struct mutex            mutex;
  spinlock_t              spin;
  struct semaphore        semaphore;
  Queueable               q[];
} QueueableBatch;

/**********************************************************************/

static void mutexSemaphoreProduce(QueueableBatch *qb,
                                  int             mySection,
                                  int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    mutex_lock(&qb->mutex);
    list_add_tail(&qb->q[i].list, &qb->list);
    mutex_unlock(&qb->mutex);
    uds_release_semaphore(&qb->semaphore);
  }
}

static void mutexSemaphoreConsume(QueueableBatch *qb)
{
  long i;
  for (i = 0; i < qb->count; i++) {
    uds_acquire_semaphore(&qb->semaphore);
    mutex_lock(&qb->mutex);
    Queueable *q = list_first_entry_or_null(&qb->list, Queueable, list);
    CU_ASSERT_PTR_NOT_NULL(q);
    list_del(&q->list);
    mutex_unlock(&qb->mutex);
  }
}

/**********************************************************************/

static void mutexCompletionProduce(QueueableBatch *qb,
                                   int             mySection,
                                   int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    mutex_lock(&qb->mutex);
    list_add_tail(&qb->q[i].list, &qb->list);
    if (qb->active++ == 0) {
      complete(&qb->wait);
    }
    mutex_unlock(&qb->mutex);
  }
}

static void mutexCompletionConsume(QueueableBatch *qb)
{
  bool waitFlag = true;
  long i;
  for (i = 0; i < qb->count; i++) {
    if (waitFlag) {
      wait_for_completion(&qb->wait);
    }
    mutex_lock(&qb->mutex);
    Queueable *q = list_first_entry_or_null(&qb->list, Queueable, list);
    CU_ASSERT_PTR_NOT_NULL(q);
    list_del(&q->list);
    waitFlag = (--qb->active == 0);
    if (waitFlag) {
      reinit_completion(&qb->wait);
    }
    mutex_unlock(&qb->mutex);
  }
}

/**********************************************************************/

static void spinSemaphoreProduce(QueueableBatch *qb,
                                 int             mySection,
                                 int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    unsigned long flags;
    spin_lock_irqsave(&qb->spin, flags);
    list_add_tail(&qb->q[i].list, &qb->list);
    spin_unlock_irqrestore(&qb->spin, flags);
    uds_release_semaphore(&qb->semaphore);
  }
}

static void spinSemaphoreConsume(QueueableBatch *qb)
{
  long i;
  for (i = 0; i < qb->count; i++) {
    uds_acquire_semaphore(&qb->semaphore);
    unsigned long flags;
    spin_lock_irqsave(&qb->spin, flags);
    Queueable *q = list_first_entry_or_null(&qb->list, Queueable, list);
    CU_ASSERT_PTR_NOT_NULL(q);
    list_del(&q->list);
    spin_unlock_irqrestore(&qb->spin, flags);
  }
}

/**********************************************************************/

static void spinCompletionProduce(QueueableBatch *qb,
                                  int             mySection,
                                  int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    unsigned long flags;
    spin_lock_irqsave(&qb->spin, flags);
    list_add_tail(&qb->q[i].list, &qb->list);
    if (qb->active++ == 0) {
      complete(&qb->wait);
    }
    spin_unlock_irqrestore(&qb->spin, flags);
  }
}

static void spinCompletionConsume(QueueableBatch *qb)
{
  bool waitFlag = true;
  long i;
  for (i = 0; i < qb->count; i++) {
    if (waitFlag) {
      wait_for_completion(&qb->wait);
    }
    unsigned long flags;
    spin_lock_irqsave(&qb->spin, flags);
    Queueable *q = list_first_entry_or_null(&qb->list, Queueable, list);
    CU_ASSERT_PTR_NOT_NULL(q);
    list_del(&q->list);
    waitFlag = (--qb->active == 0);
    if (waitFlag) {
      reinit_completion(&qb->wait);
    }
    spin_unlock_irqrestore(&qb->spin, flags);
  }
}

/**********************************************************************/

static void llistSemaphoreProduce(QueueableBatch *qb,
                                  int             mySection,
                                  int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    llist_add(&qb->q[i].llist, &qb->llist);
    uds_release_semaphore(&qb->semaphore);
  }
}

static void llistSemaphoreConsume(QueueableBatch *qb)
{
  long i = 0;
  while (i < qb->count) {
    uds_acquire_semaphore(&qb->semaphore);
    struct llist_node *head = llist_del_all(&qb->llist);
    head = llist_reverse_order(head);
    bool first = true;
    struct queueable *entry, *temp;
    llist_for_each_entry_safe(entry, temp, head, llist) {
      if (first) {
        first = false;
      } else {
        // Keep semaphore and entry counts in sync.
        uds_acquire_semaphore(&qb->semaphore);
      }
      i++;
    }
  }
  CU_ASSERT_TRUE(i == qb->count);
  CU_ASSERT_TRUE(llist_empty(&qb->llist));
}

/**********************************************************************/

static void llistWaitqueueProduce(QueueableBatch *qb,
                                  int             mySection,
                                  int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    llist_add(&qb->q[i].llist, &qb->llist);
    if (waitqueue_active(&qb->wqhead)) {
      wake_up(&qb->wqhead);
    }
  }
}

static void llistWaitqueueConsume(QueueableBatch *qb)
{
  long i = 0;
  while (i < qb->count) {
    wait_event(qb->wqhead, !llist_empty(&qb->llist));
    struct llist_node *head = llist_del_all(&qb->llist);
    head = llist_reverse_order(head);
    struct queueable *entry, *temp;
    llist_for_each_entry_safe(entry, temp, head, llist) {
      i++;
    }
  }
  CU_ASSERT_TRUE(i == qb->count);
  CU_ASSERT_TRUE(llist_empty(&qb->llist));
}

/**********************************************************************/

static void llistEventProduce(QueueableBatch *qb, int mySection, int sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    llist_add(&qb->q[i].llist, &qb->llist);
    event_count_broadcast(qb->event);
  }
}

static void llistEventConsume(QueueableBatch *qb)
{
  long i = 0;
  while (i < qb->count) {
    struct llist_node *head;
    if (llist_empty(&qb->llist)) {
      event_token_t token = event_count_prepare(qb->event);
      if (!llist_empty(&qb->llist)) {
        event_count_cancel(qb->event, token);
      } else {
        event_count_wait(qb->event, token, NULL);
      }
    }
    head = llist_del_all(&qb->llist);
    head = llist_reverse_order(head);
    struct queueable *entry, *temp;
    llist_for_each_entry_safe(entry, temp, head, llist) {
      i++;
    }
  }
  CU_ASSERT_TRUE(llist_empty(&qb->llist));
  CU_ASSERT_TRUE(i == qb->count);
}

/**********************************************************************/

static void funnelSemaphoreProduce(QueueableBatch *qb,
                                   int             mySection,
                                   int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    vdo_funnel_queue_put(qb->funnel, &qb->q[i].funnel);
    uds_release_semaphore(&qb->semaphore);
  }
}

static void funnelSemaphoreConsume(QueueableBatch *qb)
{
  // How many entries have we consumed?
  long i = 0;
  // How many have we been told have been enqueued that we haven't consumed?
  long dequeued = 0;
  for (i = 0; i < qb->count; i++) {
    struct funnel_queue_entry *fq;
    uds_acquire_semaphore(&qb->semaphore);
    while ((fq = vdo_funnel_queue_poll(qb->funnel)) != NULL) {
      dequeued++;
    }
  }
  CU_ASSERT_TRUE(dequeued == qb->count);
}

/**********************************************************************/

static void funnelWaitqueueProduce(QueueableBatch *qb,
                                   int             mySection,
                                   int             sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    vdo_funnel_queue_put(qb->funnel, &qb->q[i].funnel);
    if (wq_has_sleeper(&qb->wqhead)) {
      wake_up(&qb->wqhead);
    }
  }
}

static void funnelWaitqueueConsume(QueueableBatch *qb)
{
  long i;
  for (i = 0; i < qb->count; i++) {
    // wait_event completes only if the condition holds; no assertion needed
    wait_event(qb->wqhead, vdo_funnel_queue_poll(qb->funnel));
  }
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(qb->funnel));
}

/**********************************************************************/

static void funnelEventProduce(QueueableBatch *qb, int mySection, int sections)
{
  long count = qb->count / sections;
  long i;
  for (i = mySection * count; i < (mySection + 1) * count; i++) {
    qb->q[i].stream = qb->stream;
    qb->q[i].number = i;
    vdo_funnel_queue_put(qb->funnel, &qb->q[i].funnel);
    event_count_broadcast(qb->event);
  }
}

static void funnelEventConsume(QueueableBatch *qb)
{
  long i = 0;
  while (i < qb->count) {
    struct funnel_queue_entry *fq = vdo_funnel_queue_poll(qb->funnel);
    if (fq != NULL) {
      i++;
      continue;
    }
    event_token_t token = event_count_prepare(qb->event);
    fq = vdo_funnel_queue_poll(qb->funnel);
    if (fq != NULL) {
      event_count_cancel(qb->event, token);
      i++;
      continue;
    }
    event_count_wait(qb->event, token, NULL);
    // Back to the top where we check again, without automatically updating i.
    // (Guard against a temporarily disconnected queue.)
  }
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(qb->funnel));
}

/**********************************************************************/

static QueueableBatch *allocateBatch(long stream, long count)
{
  QueueableBatch *qb;
  UDS_ASSERT_SUCCESS(vdo_allocate_extended(QueueableBatch, count, Queueable,
                                           __func__, &qb));
  qb->count  = count;
  qb->stream = stream;
  qb->active = 0;
  init_completion(&qb->wait);
  INIT_LIST_HEAD(&qb->list);
  init_llist_head(&qb->llist);
  mutex_init(&qb->mutex);
  spin_lock_init(&qb->spin);
  init_waitqueue_head(&qb->wqhead);
  UDS_ASSERT_SUCCESS(uds_initialize_semaphore(&qb->semaphore, 0));
  UDS_ASSERT_SUCCESS(make_event_count(&qb->event));
  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&qb->funnel));
  return qb;
}

/**********************************************************************/

static void freeBatch(QueueableBatch *qb)
{
  free_event_count(qb->event);
  vdo_free_funnel_queue(qb->funnel);
  vdo_free(qb);
}

/**********************************************************************/
static void reportTime(const char *label,
                       const char *type,
                       ktime_t     time,
                       long        count)
{
  char *printTime;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&printTime, time / count));
  albPrint("    %-10s %s/%s", label, printTime, type);
  vdo_free(printTime);
}

/**********************************************************************/

typedef struct threadData {
  int             mySection;
  int             sections;
  void          (*producer)(QueueableBatch *qb, int mySection, int sections);
  struct thread  *id;
  QueueableBatch *qb;
} ThreadData;

/**********************************************************************/

static void producerThread(void *p)
{
  ThreadData *td = p;
  td->producer(td->qb, td->mySection, td->sections);
}

/**********************************************************************/

static void testQuadTime(const char *label,
                         void producer(QueueableBatch *qb,
                                       int mySection,
                                       int sections),
                         void consumer(QueueableBatch *qb))
{
  int i;
  QueueableBatch *qb = allocateBatch(1, 5000000);
  ThreadData td[4];
  ktime_t startTime = current_time_ns(CLOCK_REALTIME);
  for (i = 0; i < 4; i++) {
    td[i].mySection = i;
    td[i].sections  = 4;
    td[i].producer  = producer;
    td[i].qb        = qb;
    UDS_ASSERT_SUCCESS(vdo_create_thread(producerThread, &td[i], "producer",
                                         &td[i].id));
  }
  consumer(qb);
  ktime_t quadTime = ktime_sub(current_time_ns(CLOCK_REALTIME), startTime);
  reportTime(label, "quad", quadTime, qb->count);
  for (i = 0; i < 4; i++) {
    vdo_join_threads(td[i].id);
  }
  freeBatch(qb);
}

/**********************************************************************/

static void quadTest(void)
{
  testQuadTime("Mutex+Sem",  mutexSemaphoreProduce,  mutexSemaphoreConsume);
  testQuadTime("Mutex+Comp", mutexCompletionProduce, mutexCompletionConsume);
  testQuadTime("Spin+Sem",   spinSemaphoreProduce,   spinSemaphoreConsume);
  testQuadTime("Spin+Comp",  spinCompletionProduce,  spinCompletionConsume);
  testQuadTime("Funnel+Sem", funnelSemaphoreProduce, funnelSemaphoreConsume);
  testQuadTime("Funnel+WQ",  funnelWaitqueueProduce, funnelWaitqueueConsume);
  testQuadTime("Funnel+Ev",  funnelEventProduce,     funnelEventConsume);
  testQuadTime("Llist+Sem",  llistSemaphoreProduce,  llistSemaphoreConsume);
  testQuadTime("Llist+WQ",   llistWaitqueueProduce,  llistWaitqueueConsume);
  testQuadTime("Llist+Ev",   llistEventProduce,      llistEventConsume);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Timing",     quadTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "RequestQueue_p1",
  .tests = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
