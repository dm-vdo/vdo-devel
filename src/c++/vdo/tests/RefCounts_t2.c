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
#include "time-utils.h"

#include "block-allocator.h"
#include "journal-point.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "status-codes.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  SLAB_SIZE    = (1 << 23),
  COUNT        = 100000,
  JOURNAL_SIZE = 2,
};

static struct ref_counts      *refs;
static struct slab_depot      *depot;
static struct block_allocator  allocator;
static struct vdo_slab        *slab;

/**********************************************************************/
static void initializeRefCounts(void)
{
  srand(42);
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE_EXTENDED(struct slab_depot, 1,
                                           struct block_allocator *,
                                           __func__, &depot));
  depot->allocators[0] = &allocator;
  allocator.depot      = depot;

  VDO_ASSERT_SUCCESS(vdo_configure_slab(SLAB_SIZE, JOURNAL_SIZE,
                                        &depot->slab_config));
  VDO_ASSERT_SUCCESS(vdo_make_slab(0, &allocator, 0, NULL, 0, false, &slab));
  VDO_ASSERT_SUCCESS(vdo_allocate_ref_counts_for_slab(slab));

  /*
   * Set the slab to be unrecovered so that slab journal locks will be ignored.
   * Since this test doesn't maintain the correct lock invariants, it would
   * fail on a lock count underflow otherwise.
   */
  vdo_mark_slab_unrecovered(slab);
  refs = slab->reference_counts;
}

/**********************************************************************/
static void tearDownRefCounts(void)
{
  UDS_FREE(depot);
  vdo_free_slab(UDS_FORGET(slab));
}

/**
 * Set a PBN to have a given number of references.
 *
 * @param pbn   The physical block number to modify
 * @param value The reference count to give the block
 **/
static void setReferenceCount(physical_block_number_t pbn, size_t value)
{
  enum reference_status      refStatus;
  bool                       wasFree;
  struct reference_operation operation = {
    .pbn  = pbn,
    .type = VDO_JOURNAL_DATA_DECREMENT,
  };
  VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &refStatus));
  while (refStatus == RS_SHARED) {
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &wasFree));
    VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &refStatus));
  }
  if (refStatus == RS_SINGLE) {
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &wasFree));
    VDO_ASSERT_SUCCESS(vdo_get_reference_status(refs, pbn, &refStatus));
  }
  CU_ASSERT_EQUAL(refStatus, RS_FREE);
  operation.type = VDO_JOURNAL_DATA_INCREMENT;
  for (size_t i = 0; i < value; i++) {
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &wasFree));
  }
}

/**
 * Time the amount of time it takes to find blocks, and clean up.
 **/
static void performanceTest(block_count_t blocks)
{
  block_count_t freeBlocks = vdo_count_unreferenced_blocks(refs, 0, blocks);
  uint64_t   elapsed    = current_time_us();
  for (block_count_t i = 0; i < freeBlocks; i++) {
    physical_block_number_t pbn;
    VDO_ASSERT_SUCCESS(vdo_allocate_unreferenced_block(refs, &pbn));
    CU_ASSERT_TRUE(pbn < blocks);
  }

  elapsed = current_time_us() - elapsed;
  printf("(%lu free in %lu usec) ", freeBlocks, elapsed);

  CU_ASSERT_EQUAL(0, vdo_count_unreferenced_blocks(refs, 0, blocks));
}

/**
 * Allocate a 100000-element empty refcount array.
 **/
static void testEmptyArray(void)
{
  performanceTest(COUNT);
}

/**
 * Allocate a 100000-element refcount array, assign random values, then time
 * finding free blocks.
 **/
static void testVeryFullArray(void)
{
  for (size_t k = 0; k < COUNT; k++) {
    setReferenceCount(k, random() % 16);
  }
  performanceTest(COUNT);
}

/**
 * Allocate a 100000-element refcount array, and make it 90% free space.
 **/
static void testMostlyEmptyArray(void)
{
  for (size_t k = 0; k < COUNT / 10; k++) {
    size_t index = random() % COUNT;
    setReferenceCount(index, random() % 16);
  }
  performanceTest(COUNT);
}

/**
 * Allocate a 100000-element refcount array and make it 90% used space.
 **/
static void testMostlyFullArray(void)
{
  for (size_t k = 0; k < COUNT; k++) {
    setReferenceCount(k, random() % 16);
  }
  for (size_t k = 0; k < COUNT / 10; k++) {
    size_t index = random() % COUNT;
    setReferenceCount(index, 0);
  }
  performanceTest(COUNT);
}

/**
 * Test a full slab except for the last block.
 **/
static void testFullArray(void)
{
  // Incref all blocks except the last.
  block_count_t dataBlocks = depot->slab_config.data_blocks;
  for (size_t k = 1; k < dataBlocks - 1; k++) {
    setReferenceCount(k, 1);
  }
  performanceTest(dataBlocks);
}

/**
 * Test all free block positions are found correctly for a given refcount
 * array length.
 *
 * @param length        The refcount array length to test
 **/
static void testAllFreeBlockPositions(block_count_t arrayLength)
{
  // Make all counts 1.
  bool wasFree;
  for (size_t k = 0; k < arrayLength; k++) {
    setReferenceCount(k, 1);
  }

  // Try every free block position. PBNs and array indexes can be directly
  // compared here since they both start at zero in the test configuration.
  for (physical_block_number_t freePBN = 1; freePBN < arrayLength; freePBN++) {
    // Adjust the previously-free block to 1, and the new free one to 0.
    struct reference_operation operation = {
      .pbn  = freePBN - 1,
      .type = VDO_JOURNAL_DATA_INCREMENT,
    };
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &wasFree));
    operation = (struct reference_operation) {
      .pbn  = freePBN,
      .type = VDO_JOURNAL_DATA_DECREMENT,
    };
    VDO_ASSERT_SUCCESS(vdo_adjust_reference_count(refs, operation, NULL,
                                                  &wasFree));

    // Test that the free block is found correctly for all starts and ends.
    for (size_t start = 0; start < arrayLength; start++) {
      for (size_t end = start; end <= arrayLength; end++) {
        bool inRange = ((start <= freePBN) && (freePBN < end));
        slab_block_number freeIndex;
        if (vdo_find_free_block(refs, start, end, &freeIndex)) {
          CU_ASSERT_TRUE(inRange);
          CU_ASSERT_EQUAL(freePBN, freeIndex);
        } else {
          CU_ASSERT_FALSE(inRange);
        }
      }
    }
  }
}

/**
 * The octet code kicks in at 32 refcounts. Test all possible single
 * free block locations for refcount arrays of length 32 to 96, to ensure all
 * reasonable corner cases of the octet code are caught.
 **/
static void testAllSmallArrays(void)
{
  for (size_t size = 32; size < 96; size++) {
    testAllFreeBlockPositions(size);
  }
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "0% full array",           testEmptyArray      },
  { "10% full array",          testMostlyEmptyArray},
  { "90% full array",          testMostlyFullArray },
  { "99.6% full array",        testVeryFullArray   },
  { "100% full slab",          testFullArray       },
  { "all small arrays",        testAllSmallArrays  },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                   = "Reference counter speed tests (RefCounts_t2)",
  .initializer            = initializeRefCounts,
  .cleaner                = tearDownRefCounts,
  .tests                  = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
