/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <stdlib.h>

#include "memory-alloc.h"
#include "numeric.h"
#include "syscalls.h"
#include "time-utils.h"

#include "priority-table.h"

#include "vdoAsserts.h"

enum {
  MAX_PRIORITY = 63
};

typedef struct {
  struct list_head priorityNode;
  unsigned int     priority;
} QueueEntry;

static struct priority_table *table = NULL;

/**********************************************************************/
static void setUp(void)
{
  VDO_ASSERT_SUCCESS(vdo_make_priority_table(MAX_PRIORITY, &table));
}

/**********************************************************************/
static void tearDown(void)
{
  vdo_free_priority_table(vdo_forget(table));
}

/**
 * Construct and initialize new QueueEntry with a specified priority.
 *
 * @param entry     The queue entry to initialize
 * @param priority  The priority to assign to the entry
 **/
static void initializeEntry(QueueEntry *entry, unsigned int priority)
{
  entry->priority = priority;
  INIT_LIST_HEAD(&entry->priorityNode);
}

/**
 * Initialize new QueueEntry with a random priority.
 *
 * @param entry  The queue entry to initialize
 *
 * @return the new entry
 **/
static void initializeRandomEntry(QueueEntry *entry)
{
  initializeEntry(entry, random() % (MAX_PRIORITY + 1));
}

/**********************************************************************/
static void enqueue(QueueEntry *entry)
{
  vdo_priority_table_enqueue(table, entry->priority, &entry->priorityNode);
}

/**
 * Deqeueue all the entries in the priority_table, checking that they come out
 * ordered by priority.
 *
 * @param entryCount   The expected number of entries in the table
 **/
static void drainTable(size_t entryCount)
{
  unsigned int lastPriority = MAX_PRIORITY;

  for (size_t i = 0; i < entryCount; i++) {
    QueueEntry *entry = (QueueEntry *) vdo_priority_table_dequeue(table);
    CU_ASSERT_PTR_NOT_NULL(entry);
    CU_ASSERT_TRUE(entry->priority <= lastPriority);
    lastPriority = entry->priority;
  }

  CU_ASSERT_PTR_NULL(vdo_priority_table_dequeue(table));
  CU_ASSERT_TRUE(vdo_is_priority_table_empty(table));
}

/**********************************************************************/
static void testEmptyTable(void)
{
  // Verify that a new table is actually empty.
  drainTable(0);

  // Reset an already-empty table. It should remain empty.
  vdo_reset_priority_table(table);
  drainTable(0);
}

/**********************************************************************/
static void testSingletonTable(void)
{
  // Enqueue one entry with a randomly-selected priority.
  QueueEntry entry;
  initializeRandomEntry(&entry);
  enqueue(&entry);
  CU_ASSERT_FALSE(vdo_is_priority_table_empty(table));

  // Dequeue it.
  CU_ASSERT_PTR_EQUAL(&entry, vdo_priority_table_dequeue(table));

  // The table must now be empty.
  drainTable(0);

  // Enqueue and dequeue the already-used entry again.
  enqueue(&entry);
  CU_ASSERT_PTR_EQUAL(&entry, vdo_priority_table_dequeue(table));
  drainTable(0);

  // Enqueue and then explicitly remove the entry from the table.
  enqueue(&entry);
  vdo_priority_table_remove(table, &entry.priorityNode);
  drainTable(0);

  // Enqueue the entry, then reset the table to clear it out.
  enqueue(&entry);
  vdo_reset_priority_table(table);
  drainTable(0);

  // Put the entry back in to make sure that the reset left everything in a
  // usable state.
  enqueue(&entry);
  drainTable(1);
}

/**********************************************************************/
static void enqueueEntries(QueueEntry entries[], size_t count)
{
  for (size_t i = 0; i < count; i++) {
    enqueue(&entries[i]);
  }
}

/**********************************************************************/
static void testRandomTable(void)
{
  // Construct and initialize an array of a million random entries.
  enum { COUNT = 1000 * 1000 };

  QueueEntry *entries;
  VDO_ASSERT_SUCCESS(vdo_allocate(COUNT, __func__, &entries));

  for (size_t i = 0; i < COUNT; i++) {
    initializeRandomEntry(&entries[i]);
  }

  // Time how long it takes to fill the table.
  long elapsed = -current_time_us();
  enqueueEntries(entries, COUNT);
  elapsed += current_time_us();
  printf("\n%d entries enqueued in %8ld microseconds (%3ld ns ea)\n",
         COUNT, elapsed, elapsed * 1000 / COUNT);

  // Time how long it takes to empty the table.
  elapsed = -current_time_us();
  drainTable(COUNT);
  elapsed += current_time_us();
  printf("%d entries dequeued in %8ld microseconds (%3ld ns ea)\n",
         COUNT, elapsed, elapsed * 1000 / COUNT);

  // Add all the entries again, one by one, and after each entry, confirm
  // that dequeue will return the highest priority entry in the table.
  enqueue(&entries[0]);
  unsigned int topPriority = entries[0].priority;
  for (size_t i = 1; i < COUNT; i++) {
    enqueue(&entries[i]);
    topPriority = max(topPriority, entries[i].priority);

    QueueEntry *entry = (QueueEntry *) vdo_priority_table_dequeue(table);
    CU_ASSERT_EQUAL(topPriority, entry->priority);
    enqueue(entry);
  }

  // Remove the even-numbered entries from the table.
  for (size_t i = 0; i < COUNT; i += 2) {
    vdo_priority_table_remove(table, &entries[i].priorityNode);
  }

  // Verify that only odd-numbered entries remain in the table.
  unsigned int lastPriority = MAX_PRIORITY;
  for (size_t i = 1; i < COUNT; i += 2) {
    QueueEntry *entry = (QueueEntry *) vdo_priority_table_dequeue(table);
    size_t entryIndex = entry - entries;
    CU_ASSERT_TRUE((entryIndex % 2) == 1);
    CU_ASSERT_TRUE(entry->priority <= lastPriority);
    lastPriority = entry->priority;
  }

  // The table must now be empty again.
  drainTable(0);

  // Add one entry, drain, the next two entries, drain, the next three
  // entries, drain, etc, until all the entries have been used once.
  size_t nextIndex = 0;
  size_t batch     = 0;
  while (nextIndex < COUNT) {
    batch = min(batch + 1, COUNT - nextIndex);
    enqueueEntries(&entries[nextIndex], batch);
    nextIndex += batch;
    drainTable(batch);
  }

  // The table must now be empty again.
  drainTable(0);

  // Put all the entries in, reset the table, then put them all in again.
  enqueueEntries(entries, COUNT);
  vdo_reset_priority_table(table);
  drainTable(0);

  // Cycle all the entries through the table again to make sure that the reset
  // left everything in a usable state.
  enqueueEntries(entries, COUNT);
  drainTable(COUNT);

  vdo_free(entries);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "empty table",        testEmptyTable        },
  { "singleton table",    testSingletonTable    },
  { "random table",       testRandomTable       },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "PriorityTable_t1",
  .initializerWithArguments = NULL,
  .initializer              = setUp,
  .cleaner                  = tearDown,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
