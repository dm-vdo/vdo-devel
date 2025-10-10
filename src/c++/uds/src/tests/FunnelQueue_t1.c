// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * Simple units test of funnel queue. These tests exercise the functionality of
 * funnel queue in simple ways. They do not attempt to explicitly exercise all
 * possible multi-threaded interactions.
 **/

#include <linux/cache.h>

#include "albtest.h"
#include "assertions.h"
#include "funnel-queue.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "thread-utils.h"

enum {
  ITERATIONS = 200 * 1000
};

typedef struct {
  struct funnel_queue_entry link;
  uint64_t         value;
} Entry;

/**********************************************************************/
static inline void assertCacheAligned(const volatile void *address)
{
  CU_ASSERT_EQUAL(0, (uintptr_t) address & (L1_CACHE_BYTES - 1));
}

/**********************************************************************/
static void testFieldAlignment(void)
{
  struct funnel_queue *queue;
  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue));
  assertCacheAligned(queue);
  assertCacheAligned(&queue->newest);
  assertCacheAligned(&queue->oldest);
  vdo_free_funnel_queue(queue);
}

/**********************************************************************/
static void testEmptyQueue(void)
{
  struct funnel_queue *queue;
  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue));
  int i;
  for (i = 0; i < 10; i++) {
    CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));
  }
  vdo_free_funnel_queue(queue);
}

/**********************************************************************/
static void testSingletonQueue(void)
{
  struct funnel_queue *queue;
  struct funnel_queue_entry first, second;

  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue));
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  // Test the empty to non-empty transitions: 0->1, 1->0
  vdo_funnel_queue_put(queue, &first);
  CU_ASSERT_PTR_EQUAL(&first, vdo_funnel_queue_poll(queue));
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  // Do it again, making sure the new "empty" state is as good as new queue.
  vdo_funnel_queue_put(queue, &first);
  CU_ASSERT_PTR_EQUAL(&first, vdo_funnel_queue_poll(queue));
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  // Test the singleton to doubleton transitions: 0->1, 1->2, 2->1, 1->0
  vdo_funnel_queue_put(queue, &first);
  vdo_funnel_queue_put(queue, &second);
  CU_ASSERT_PTR_EQUAL(&first, vdo_funnel_queue_poll(queue));
  CU_ASSERT_PTR_EQUAL(&second, vdo_funnel_queue_poll(queue));
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  vdo_free_funnel_queue(queue);
}

/**
 * Thread function that loops ITERATIONS times, putting newly allocated Entry
 * instances with values 0 .. ITERATIONS - 1 on the funnel queue passed as the
 * opaque function argument.
 **/
static void enqueueLoop(void *arg)
{
  struct funnel_queue *queue = (struct funnel_queue *) arg;
  unsigned int i;
  for (i = 0; i < ITERATIONS; i++) {
    Entry *entry;
    UDS_ASSERT_SUCCESS(vdo_allocate(1, __func__, &entry));
    entry->value = i;
    vdo_funnel_queue_put(queue, &entry->link);
  }
}

/**
 * Remove an Entry from a funnel queue, looping and sleeping if the queue
 * appears to be empty.
 **/
static Entry *dequeue(struct funnel_queue *queue)
{
  for (;;) {
    Entry *entry = (Entry *) vdo_funnel_queue_poll(queue);
    if (entry != NULL) {
      return entry;
    }
    sleep_for(us_to_ktime(1));
  }
}

/**
 * Exercise a single producer thread generating ITERATIONS entries, all
 * consumed by the test thread.
 **/
static void testOneProducer(void)
{
  struct funnel_queue *queue;
  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue));

  // Start a single thread to generate ITERATIONS queue Entry instances.
  struct thread *producer;
  UDS_ASSERT_SUCCESS(vdo_create_thread(enqueueLoop, queue, "producer",
                                       &producer));

  // Consume the entries, which should be in numeric order since there's
  // just a single producer thread.
  unsigned int i;
  for (i = 0; i < ITERATIONS; i++) {
    Entry *entry = dequeue(queue);
    CU_ASSERT_EQUAL(entry->value, i);
    vdo_free(entry);
  }

  vdo_join_threads(producer);

  // There mustn't be any excess entries on the queue.
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  vdo_free_funnel_queue(queue);
}

/**
 * Exercise ten producer threads each generating ITERATIONS entries, all
 * consumed by the test thread.
 **/
static void testTenProducers(void)
{
  struct funnel_queue *queue;
  UDS_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue));

  // Start ten threads to generate ITERATIONS queue Entry instances.
  enum { PRODUCER_COUNT = 10 };
  struct thread *producers[PRODUCER_COUNT];
  unsigned int i;
  for (i = 0; i < PRODUCER_COUNT; i++) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(vdo_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                         "producer%d", i));
    UDS_ASSERT_SUCCESS(vdo_create_thread(enqueueLoop, queue, nameBuf,
                                         &producers[i]));
  }

  // Allocate an array to keep track of how many entries of each value have
  // been seen.
  u8 *seen;
  UDS_ASSERT_SUCCESS(vdo_allocate(ITERATIONS, __func__, &seen));

  // Consume all the entries, accounting for the values seen.
  for (i = 0; i < ITERATIONS * PRODUCER_COUNT; i++) {
    Entry *entry = dequeue(queue);
    seen[entry->value] += 1;
    vdo_free(entry);
  }

  // Verify that each Entry value was seen 10 times for 10 threads.
  for (i = 0; i < ITERATIONS; i++) {
    CU_ASSERT_EQUAL(PRODUCER_COUNT, seen[i]);
  }
  vdo_free(seen);

  for (i = 0; i < PRODUCER_COUNT; i++) {
    vdo_join_threads(producers[i]);
  }

  // There mustn't be any excess entries on the queue.
  CU_ASSERT_PTR_NULL(vdo_funnel_queue_poll(queue));

  vdo_free_funnel_queue(queue);
}

/**********************************************************************/

static const CU_TestInfo funnelQueueTests[] = {
  {"field alignment",          testFieldAlignment  },
  {"empty queue",              testEmptyQueue      },
  {"singleton queue",          testSingletonQueue  },
  {"one producer",             testOneProducer     },
  {"ten producers",            testTenProducers    },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "FunnelQueue_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = funnelQueueTests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
