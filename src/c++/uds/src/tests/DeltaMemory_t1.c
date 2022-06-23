// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"

static const unsigned int MEAN_DELTA = 4096;
static const unsigned int NUM_PAYLOAD_BITS = 10;
static struct delta_memory dm;

/**
 * Initialize lists evenly, all memory is free
 */
static void initEvenly(const struct delta_memory *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  memset(pdl, 0, (pdm->num_lists + 1) * sizeof(struct delta_list));
  size_t usableBytes = pdl[pdm->num_lists + 1].start_offset / CHAR_BIT;
  size_t spacing = usableBytes / (pdm->num_lists + 1);
  unsigned int i;
  for (i = 0; i <= pdm->num_lists; i++) {
    pdl[i].start_offset = i * spacing * CHAR_BIT;
    pdl[i].size = 0;
  }
  validateDeltaLists(pdm);
}

/**
 * Initialize lists evenly, using all of the memory
 */
static void initFully(const struct delta_memory *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  memset(pdl, 0, (pdm->num_lists + 1) * sizeof(struct delta_list));
  size_t usableBytes = pdl[pdm->num_lists + 1].start_offset / CHAR_BIT;
  size_t spacing = usableBytes / pdm->num_lists * CHAR_BIT;
  pdl[0].start_offset = 0;
  pdl[0].size = 0;
  unsigned int i;
  for (i = 1; i <= pdm->num_lists; i++) {
    pdl[i].start_offset = pdl[i - 1].start_offset + pdl[i - 1].size;
    pdl[i].size = spacing;
  }
  pdl[pdm->num_lists].size = (pdm->size * CHAR_BIT
                             - pdl[pdm->num_lists].start_offset);
  pdl[pdm->num_lists].size = (pdl[pdm->num_lists + 1].start_offset
                             - pdl[pdm->num_lists].start_offset);
  validateDeltaLists(pdm);
}

/**
 * Allocate random space
 */
static void allocateRandomly(const struct delta_memory *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->num_lists; i++) {
    int j = ((unsigned int) random()
             % ((pdl[i + 1].start_offset - pdl[i].start_offset) / CHAR_BIT));
    pdl[i].size = j * CHAR_BIT;
    CU_ASSERT_TRUE(pdl[i].start_offset + pdl[i].size
                     <= pdl[i + 1].start_offset);
  }
  validateDeltaLists(pdm);
}

/**
 * Allocate triangular space (the Nth list is longer than the N-1st list)
 */
static void allocateTriangularly(const struct delta_memory *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->num_lists; i++) {
    pdl[i].size = i * CHAR_BIT;
    CU_ASSERT_TRUE(pdl[i].start_offset + pdl[i].size
                     <= pdl[i + 1].start_offset);
  }
  validateDeltaLists(pdm);
}

/**
 * Allocate reversed triangular space (the Nth list is shorter than the
 * N-1st list
 */
static void allocateReverseTriangularly(const struct delta_memory *pdm)
{
  struct delta_list *pdl = pdm->delta_lists;
  unsigned int i;
  for (i = 1; i <= pdm->num_lists; i++) {
    int j = pdm->num_lists + 1 - i;
    pdl[i].size = j * CHAR_BIT;

    CU_ASSERT_TRUE(pdl[i].start_offset + pdl[i].size
                     <= pdl[i + 1].start_offset);
  }
  validateDeltaLists(pdm);
}

/**
 * Store predictable data into each list
 */
static void storeData(const struct delta_memory *pdm)
{
  const struct delta_list *pdl = pdm->delta_lists;
  unsigned int i, j;
  for (i = 1; i <= pdm->num_lists; i++) {
    uint64_t offset = pdl[i].start_offset / CHAR_BIT;
    for (j = 0; j < pdl[i].size / CHAR_BIT; j++) {
      pdm->memory[offset + j] = (byte)(i + j);
    }
  }
}

/**
 * Verify the predictable data
 */
static void verifyData(const struct delta_memory *pdm)
{
  const struct delta_list *pdl = pdm->delta_lists;
  unsigned int i, j;
  for (i = 1; i <= pdm->num_lists; i++) {
    uint64_t offset = pdl[i].start_offset / CHAR_BIT;
    for (j = 0; j < pdl[i].size / CHAR_BIT; j++) {
      CU_ASSERT_EQUAL(pdm->memory[offset + j], (byte)(i + j));
    }
  }
}

/**
 * Verify the unused spacing of the rebalanced delta memory
 */
static void verifyEvenSpacing(const struct delta_memory *pdm,
                              unsigned int growingIndex, int growingSize)
{
  const struct delta_list *pdl = pdm->delta_lists;
  size_t expectedGap = 0, firstGap = 0;
  unsigned int i;
  for (i = 1; i <= pdm->num_lists + 1; i++) {
    size_t gap
      = (pdl[i].start_offset / CHAR_BIT
         - (pdl[i - 1].start_offset / CHAR_BIT + pdl[i - 1].size / CHAR_BIT));
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
    } else if (i <= pdm->num_lists) {
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
  UDS_ASSERT_SUCCESS(initialize_delta_memory(&dm, initSize, 0, nLists,
                                             MEAN_DELTA, NUM_PAYLOAD_BITS));
  // Use lists that increase in size.
  initEvenly(&dm);
  allocateTriangularly(&dm);
  // Deposit known data into the lists.
  storeData(&dm);
  verifyData(&dm);
  // Rebalance and Verify
  UDS_ASSERT_SUCCESS(extend_delta_memory(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyData(&dm);

  // Do the same test, but with lists that decrease in size.
  initEvenly(&dm);
  allocateReverseTriangularly(&dm);
  storeData(&dm);
  verifyData(&dm);
  UDS_ASSERT_SUCCESS(extend_delta_memory(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyData(&dm);

  uninitialize_delta_memory(&dm);
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
  UDS_ASSERT_SUCCESS(initialize_delta_memory(&dm, initSize, 0, nLists,
                                             MEAN_DELTA, NUM_PAYLOAD_BITS));

  // Use random list sizes.
  initEvenly(&dm);
  allocateRandomly(&dm);

  // Rebalance and verify evenness
  UDS_ASSERT_SUCCESS(extend_delta_memory(&dm, 0, 0));
  validateDeltaLists(&dm);
  verifyEvenSpacing(&dm, 0, 0);

  // Rebalance with growth and verify evenness
  unsigned int i;
  for (i = 1; i <= nLists + 1; i++) {
    UDS_ASSERT_SUCCESS(extend_delta_memory(&dm, i, i));
    validateDeltaLists(&dm);
    verifyEvenSpacing(&dm, i, i);
  }

  uninitialize_delta_memory(&dm);
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
  enum { NUM_LISTS = 1 << 10 };
  enum { ALLOC_SIZE = 1 << 17 };
  UDS_ASSERT_SUCCESS(initialize_delta_memory(&dm, ALLOC_SIZE, 0, NUM_LISTS,
                                             MEAN_DELTA, NUM_PAYLOAD_BITS));
  CU_ASSERT_EQUAL(dm.size, ALLOC_SIZE);

  // Fill and extend, expecting a UDS_OVERFLOW error
  initFully(&dm);
  UDS_ASSERT_ERROR(UDS_OVERFLOW, extend_delta_memory(&dm, 1, 1));
  CU_ASSERT_EQUAL(dm.size, ALLOC_SIZE);

  uninitialize_delta_memory(&dm);
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
