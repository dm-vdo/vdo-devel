/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <stdlib.h>

#include "assertions.h"
#include "memory-alloc.h"
#include "syscalls.h"

#include "heap.h"

typedef struct {
  uint16_t key;
  u8     value;
} __attribute__((packed)) HeapRecord;

/**********************************************************************/
static int compareRecords(const void *item1, const void *item2)
{
  const HeapRecord *record1 = (const HeapRecord *) item1;
  const HeapRecord *record2 = (const HeapRecord *) item2;
  if (record1->key < record2->key) {
    return -1;
  }
  if (record1->key > record2->key) {
    return 1;
  }
  return 0;
}

/**********************************************************************/
static void swapRecords(void *item1, void *item2)
{
  HeapRecord *record1 = item1;
  HeapRecord *record2 = item2;
  HeapRecord temp = *record1;
  *record1 = *record2;
  *record2 = temp;
}

/**********************************************************************/
static bool isSorted(const HeapRecord *records, size_t count)
{
  bool sorted = true;
  // Verify that the heap sorted the records by ascending keys.
  for (size_t i = 1; i < count; i++) {
    sorted = sorted && (records[i - 1].key <= records[i].key);
  }
  return sorted;
}

/**
 * Allocate and randomly fill an array of COUNT HeapRecords, and fill the
 * provided HeapRecord array pointer with the location.
 *
 * @param records
 * @param count    Number of records to create.
 **/
static HeapRecord *buildRandomRecords(size_t count)
{
  HeapRecord *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count, HeapRecord, __func__, &records));

  // Fill the records with random data.
  for (size_t i = 0; i < count; i++) {
    uint32_t x       = random();
    records[i].key   = (x & 0xFFFF);
    records[i].value = (x >> 16);
  }

  return records;
}

/**
 * Test the properties of a zero-capacity heap.
 **/
static void testEmptyHeap(void)
{
  struct heap heap;
  HeapRecord records[1];
  vdo_initialize_heap(&heap, compareRecords, swapRecords,
                      records, 0, sizeof(records[0]));

  // Check the properties of the empty heap.
  CU_ASSERT_TRUE(vdo_is_heap_empty(&heap));

  // There are no elements to be popped.
  HeapRecord record;
  CU_ASSERT_FALSE(vdo_pop_max_heap_element(&heap, &record));

  // Build heap does nothing, but shouldn't crash.
  vdo_build_heap(&heap, 0);

  CU_ASSERT_TRUE(vdo_is_heap_empty(&heap));
}

/**
 * Populate an array of records for a given sequence of small integers,
 * treating the keys in the records as digits in a polynomial with the
 * capacity as the base.
 *
 * @param records   The array of records to populate
 * @param capacity  The number of records to populate
 * @param sequence  The polynomial value specifying the record keys
 **/
static void fillRecords(HeapRecord records[],
                        size_t     capacity,
                        uint64_t   sequence)
{
  for (u8 i = 0; i < capacity; i++) {
    unsigned int digit = sequence % capacity;
    sequence /= capacity;
    records[i] = (HeapRecord) {
      .key   = digit,
      .value = i,
    };
  }
}

/**
 * Test every way of building a heap of the specified capacity using small
 * integer keys.
 **/
static void testSmallHeap(struct heap *heap,
                          HeapRecord   records[],
                          size_t       capacity)
{
  // Calculate (capacity ** capacity).
  uint64_t sequences = 1;
  for (size_t i = 0; i < capacity; i++) {
    sequences *= capacity;
  }

  for (uint64_t sequence = 0; sequence < sequences; sequence++) {
    // Generate the records for this unique sequence.
    fillRecords(records, capacity, sequence);

    // Copy and sort the copy of the records for reference.
    HeapRecord sortedRecords[capacity];
    memcpy(sortedRecords, records, sizeof(sortedRecords));
    qsort(sortedRecords, capacity, sizeof(HeapRecord), compareRecords);

    // Heapify the unique unsorted sequence of records in the record array.
    CU_ASSERT_TRUE(vdo_is_heap_empty(heap));
    vdo_build_heap(heap, capacity);
    CU_ASSERT_EQUAL(capacity, heap->count);

    // Pop the elements off the heap one by one, verifying that they come off
    // in order from maximum to minimum.
    uint64_t seen = 0;
    for (size_t i = 0; i < capacity; i++) {
      CU_ASSERT_EQUAL(capacity - i, heap->count);
      HeapRecord record;
      CU_ASSERT_TRUE(vdo_pop_max_heap_element(heap, &record));

      // The records are sorted in ascending order, but the heap returns them
      // in descending order.
      HeapRecord *expected = &sortedRecords[capacity - 1 - i];
      CU_ASSERT_EQUAL(expected->key, record.key);

      // The heap structure and qsort are unstable, so the values may
      // not match when two records have equal keys. Instead, keep a
      // bitset of the values we see.
      seen |= (1L << record.value);
    }

    // Make sure we saw a record with every value in 0..capacity-1
    CU_ASSERT_EQUAL((1 << capacity) - 1, seen);

    // The heap must now be empty again.
    CU_ASSERT_TRUE(vdo_is_heap_empty(heap));
    CU_ASSERT_FALSE(vdo_pop_max_heap_element(heap, NULL));
  }
}

/**
 * Test every possible way of building a heap of up to six elements with
 * small integer keys.
 **/
static void testEverySmallHeap(void)
{
  for (size_t capacity = 1; capacity <= 6; capacity++) {
    struct heap heap;
    HeapRecord records[capacity];
    vdo_initialize_heap(&heap, compareRecords, swapRecords,
                        records, capacity, sizeof(records[0]));
    testSmallHeap(&heap, records, capacity);
  }
}

/**
 * Test building a heap from 100,000 random entries and sorting the heap into
 * an array.
 **/
static void testSortHeap(void)
{
  // Allocate the record array for the heap and initialize the heap.
  static const size_t  COUNT = 100 * 1000;
  struct heap          heap;
  HeapRecord          *records = buildRandomRecords(COUNT);

  // Put the records into binary heap order.
  vdo_initialize_heap(&heap, compareRecords, swapRecords,
                  records, COUNT, sizeof(records[0]));
  vdo_build_heap(&heap, COUNT);

  // Pull records off the heap one by one, sorting them.
  CU_ASSERT_EQUAL(COUNT, vdo_sort_heap(&heap));

  // The heap should be empty now, with the records sorted in place.
  CU_ASSERT_TRUE(vdo_is_heap_empty(&heap));
  CU_ASSERT_TRUE(isSorted(records, COUNT));

  UDS_FREE(records);
}

/**
 * Test building a heap from 100,000 random entries and sorting the heap into
 * an array.
 **/
static void testSortNextHeapElement(void)
{
  // Allocate the record array for the heap and initialize the heap.
  static const size_t  COUNT = 100 * 1000;
  struct heap          heap;
  HeapRecord          *records = buildRandomRecords(COUNT);
  vdo_initialize_heap(&heap, compareRecords, swapRecords,
                  records, COUNT, sizeof(records[0]));
  // Put the records into binary heap order.
  vdo_build_heap(&heap, COUNT);

  // Pull records off the heap one by one, sorting them.
  HeapRecord *lastPulledRecord    = NULL;
  HeapRecord *currentPulledRecord = NULL;
  for (size_t i = 0; i < COUNT; i++) {
    lastPulledRecord = currentPulledRecord;
    currentPulledRecord = vdo_sort_next_heap_element(&heap);
    CU_ASSERT_TRUE(currentPulledRecord == &records[COUNT - 1 - i]);
    if (lastPulledRecord != NULL) {
      CU_ASSERT_TRUE(currentPulledRecord->key <= lastPulledRecord->key);
    }
  }

  // The heap should be empty now, with the records sorted in place.
  CU_ASSERT_TRUE(vdo_is_heap_empty(&heap));
  CU_ASSERT_TRUE(isSorted(records, COUNT));

  UDS_FREE(records);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "empty heap",         testEmptyHeap            },
  { "every small heap",   testEverySmallHeap       },
  { "sort heap",          testSortHeap             },
  { "lazily sorted heap", testSortNextHeapElement  },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "Heap_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
