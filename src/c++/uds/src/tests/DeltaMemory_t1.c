// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/bits.h>

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"

static const unsigned int MEAN_DELTA = 4096;
static const unsigned int NUM_PAYLOAD_BITS = 10;
static struct delta_zone dm;

/**
 * Initialize lists evenly, all memory is free
 */
static void initEvenly(const struct delta_zone *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  memset(pdl, 0, (pdm->list_count + 1) * sizeof(struct delta_list));
  size_t usableBytes = pdl[pdm->list_count + 1].start / BITS_PER_BYTE;
  size_t spacing = usableBytes / (pdm->list_count + 1);
  unsigned int i;
  for (i = 0; i <= pdm->list_count; i++) {
    pdl[i].start = i * spacing * BITS_PER_BYTE;
    pdl[i].size = 0;
  }
  validateDeltaLists(pdm);
}

/**
 * Initialize lists evenly, using all of the memory
 */
static void initFully(const struct delta_zone *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int list_count = pdm->list_count;
  memset(pdl, 0, (pdm->list_count + 1) * sizeof(struct delta_list));
  size_t usableBytes = pdl[list_count + 1].start / BITS_PER_BYTE;
  size_t spacing = usableBytes / pdm->list_count * BITS_PER_BYTE;
  pdl[0].start = 0;
  pdl[0].size = 0;
  unsigned int i;
  for (i = 1; i <= list_count; i++) {
    pdl[i].start = pdl[i - 1].start + pdl[i - 1].size;
    pdl[i].size = spacing;
  }
  pdl[pdm->list_count].size = (pdm->size * BITS_PER_BYTE
                               - pdl[pdm->list_count].start);
  pdl[pdm->list_count].size = (pdl[pdm->list_count + 1].start
                             - pdl[pdm->list_count].start);
  validateDeltaLists(pdm);
}

/**
 * Allocate random space
 */
static void allocateRandomly(const struct delta_zone *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->list_count; i++) {
    int j = ((unsigned int) random()
             % ((pdl[i + 1].start - pdl[i].start) / BITS_PER_BYTE));
    pdl[i].size = j * BITS_PER_BYTE;
    CU_ASSERT_TRUE(pdl[i].start + pdl[i].size <= pdl[i + 1].start);
  }
  validateDeltaLists(pdm);
}

/**
 * Allocate triangular space (the Nth list is longer than the N-1st list)
 */
static void allocateTriangularly(const struct delta_zone *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->list_count; i++) {
    pdl[i].size = i * BITS_PER_BYTE;
    CU_ASSERT_TRUE(pdl[i].start + pdl[i].size <= pdl[i + 1].start);
  }
  validateDeltaLists(pdm);
}

/**
 * Allocate reversed triangular space (the Nth list is shorter than the
 * N-1st list
 */
static void allocateReverseTriangularly(const struct delta_zone *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->list_count; i++) {
    int j = pdm->list_count + 1 - i;
    pdl[i].size = j * BITS_PER_BYTE;

    CU_ASSERT_TRUE(pdl[i].start + pdl[i].size <= pdl[i + 1].start);
  }
  validateDeltaLists(pdm);
}

/**
 * Store predictable data into each list
 */
static void storeData(const struct delta_zone *pdm)
{
  const struct delta_list *pdl = pdm->delta_lists;
  unsigned int i, j;
  for (i = 1; i <= pdm->list_count; i++) {
    uint64_t offset = pdl[i].start / BITS_PER_BYTE;
    for (j = 0; j < pdl[i].size / BITS_PER_BYTE; j++) {
      pdm->memory[offset + j] = (u8)(i + j);
    }
  }
}

/**
 * Verify the predictable data
 */
static void verifyData(const struct delta_zone *pdm)
{
  const struct delta_list *pdl = pdm->delta_lists;
  unsigned int i, j;
  for (i = 1; i <= pdm->list_count; i++) {
    uint64_t offset = pdl[i].start / BITS_PER_BYTE;
    for (j = 0; j < pdl[i].size / BITS_PER_BYTE; j++) {
      CU_ASSERT_EQUAL(pdm->memory[offset + j], (u8)(i + j));
    }
  }
}

/**
 * Verify the unused spacing of the rebalanced delta memory
 */
static void verifyEvenSpacing(const struct delta_zone *pdm,
                              unsigned int growingIndex, int growingSize)
{
  const struct delta_list *pdl = pdm->delta_lists;
  size_t expectedGap = 0, firstGap = 0;
  unsigned int i;
  for (i = 1; i <= pdm->list_count + 1; i++) {
    size_t gap = pdl[i].start / BITS_PER_BYTE
                  - (pdl[i - 1].start / BITS_PER_BYTE
                     + pdl[i - 1].size / BITS_PER_BYTE);
    // There must be space between lists
    CU_ASSERT_TRUE((int) gap > 0);
    if (i == growingIndex) {
      // This particular gap must be larger than growingSize
      CU_ASSERT_TRUE((int) gap >= growingSize);
      gap -= growingSize;
    }
    // All gaps but the first and last ones must be the same size
    if (i == 1) {
      firstGap = gap;
    } else if (i == 2) {
      expectedGap = gap;
    } else if (i <= pdm->list_count) {
      CU_ASSERT_EQUAL(gap, expectedGap);
    }
  }
  CU_ASSERT_TRUE(firstGap <= expectedGap);
}

/**
 * Test basic rebalancing
 **/
static void rebalanceTest(int nLists, int bytesPerList, int allocIncr)
{
  int initSize = ((nLists + 2) * bytesPerList / allocIncr + 1) * allocIncr;
  UDS_ASSERT_SUCCESS(initialize_delta_zone(&dm, initSize, 0, nLists,
                                           MEAN_DELTA, NUM_PAYLOAD_BITS, 'm'));
  // Use lists that increase in size.
  initEvenly(&dm);
  allocateTriangularly(&dm);
  // Deposit known data into the lists.
  storeData(&dm);
  verifyData(&dm);
  // Rebalance and Verify
  UDS_ASSERT_SUCCESS(extend_delta_zone(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyData(&dm);

  // Do the same test, but with lists that decrease in size.
  initEvenly(&dm);
  allocateReverseTriangularly(&dm);
  storeData(&dm);
  verifyData(&dm);
  UDS_ASSERT_SUCCESS(extend_delta_zone(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyData(&dm);

  uninitialize_delta_zone(&dm);
}

static void smallRebalanceTest(void)
{
  rebalanceTest(10, 10, 1 << 10);
}

static void largeRebalanceTest(void)
{
  rebalanceTest(200, 200, 1 << 10);
}

/**
 * Test evenness of balancing, both with and without growing.
 **/
static void growingTest(unsigned int nLists, int bytesPerList, int allocIncr)
{
  int initSize = ((nLists + 2) * bytesPerList / allocIncr + 1) * allocIncr;
  UDS_ASSERT_SUCCESS(initialize_delta_zone(&dm, initSize, 0, nLists,
                                           MEAN_DELTA, NUM_PAYLOAD_BITS, 'm'));

  // Use random list sizes.
  initEvenly(&dm);
  allocateRandomly(&dm);

  // Rebalance and verify evenness
  UDS_ASSERT_SUCCESS(extend_delta_zone(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyEvenSpacing(&dm, 0, 0);

  // Rebalance with growth and verify evenness
  unsigned int i;
  for (i = 1; i <= nLists + 1; i++) {
    UDS_ASSERT_SUCCESS(extend_delta_zone(&dm, i, i));
    validateDeltaLists(&dm);
    verifyEvenSpacing(&dm, i, i);
  }

  uninitialize_delta_zone(&dm);
}

static void smallGrowingTest(void)
{
  growingTest(10, 10, 1 << 10);
}

static void largeGrowingTest(void)
{
  growingTest(200, 200, 1 << 10);
}

/**
 * Test memory overflow
 **/
static void overflowTest(void)
{
  enum { LIST_COUNT = 1 << 10 };
  enum { ALLOC_SIZE = 1 << 17 };
  UDS_ASSERT_SUCCESS(initialize_delta_zone(&dm, ALLOC_SIZE, 0, LIST_COUNT,
                                           MEAN_DELTA, NUM_PAYLOAD_BITS, 'm'));
  CU_ASSERT_EQUAL(dm.size, ALLOC_SIZE);

  // Fill and extend, expecting a UDS_OVERFLOW error
  initFully(&dm);
  UDS_ASSERT_ERROR(UDS_OVERFLOW, extend_delta_zone(&dm, 1, 1));
  CU_ASSERT_EQUAL(dm.size, ALLOC_SIZE);

  uninitialize_delta_zone(&dm);
}

/**********************************************************************/

static const CU_TestInfo deltaMemoryTests[] = {
  {"Small Rebalance",    smallRebalanceTest },
  {"Large Rebalance",    largeRebalanceTest },
  {"Small Growing",      smallGrowingTest },
  {"Large Growing",      largeGrowingTest },
  {"Overflow",           overflowTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "DeltaMemory_t1",
  .tests = deltaMemoryTests
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
