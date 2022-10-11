// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 *
 * Test of steady state indexing performance.
 */

/**
 * PostBlockName_p1 (formerly Index_p3) measures the average throughput of
 * udsPostBlockName(). It times the filling phase, steady-state operation with
 * no deduplication, and steady-state operation with 30-70% deduplication.
 **/

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "indexPerfCommon.h"
#include "oldInterfaces.h"
#include "random.h"
#include "testPrototypes.h"

static struct uds_index_session *indexSession;

/**
 * Half Dedupe Pattern: 8 streams of dedupe randomly selected, skipping
 * ahead randomly <= 8 blocks on each iteration, for 2^15 blocks on average.
 * Should give average dedupe band size of 1/3.5 of chapter size and generate
 * approximately a 50% dedupe rate.
 **/
static uint64_t halfDedupe(FillState *state)
{
  enum { BITS_INDEX  = 3 };
  enum { BITS_INCR   = 3 };
  enum { BITS_RESET  = 15 };
  enum { BITS_FLAG   = 2 };
  enum { SHIFT_INCR  = BITS_INDEX };
  enum { SHIFT_RESET = SHIFT_INCR + BITS_INCR };
  enum { SHIFT_FLAG  = SHIFT_RESET + BITS_RESET };
  enum { MASK_INDEX  = (1 << BITS_INDEX) - 1 };
  enum { MASK_INCR   = (1 << BITS_INCR) - 1 };
  enum { MASK_RESET  = ((1 << BITS_RESET) - 1) << SHIFT_RESET };
  enum { MASK_FLAG   = ((1 << BITS_FLAG) - 1) << SHIFT_FLAG };
  enum { NUM_DUPE_COUNTERS = 1 << BITS_INDEX };
  static uint64_t dupeCounters[NUM_DUPE_COUNTERS];

  long int randomValue = random();
  if ((randomValue & MASK_FLAG) == 0) {
    return state->nameCounter++;
  }

  int index = randomValue & MASK_INDEX;
  if ((randomValue & MASK_RESET) == 0) {
    dupeCounters[index] = state->nameCounter;
  } else {
    unsigned int incr = (randomValue >> SHIFT_INCR) & MASK_INCR;
    dupeCounters[index] += 1 + incr;
  }
  if (dupeCounters[index] >= state->nameCounter) {
    uint64_t random64;
    get_random_bytes(&random64, sizeof(random64));
    dupeCounters[index] = random64 % state->nameCounter;
  }
  return dupeCounters[index];
}

/**********************************************************************/
static void pbnPerfTest(void)
{
  initializeOldInterfaces(2000);

  FillState state = {
    .nameCounter = 0,
    .private = NULL
  };

  // Fill the index with blocks of size 4K.
  // Split the blocks into 16M groups for a comfortable amount of logging.
  uint64_t numBlocksToWrite = getBlocksPerIndex(indexSession);
  unsigned int numBlocksPerGroup = 1 << 24;
  unsigned int numGroups
    = (unsigned int) (numBlocksToWrite / numBlocksPerGroup);
  albPrint("Fill the index with %uM chunks in %u groups of %uM chunks",
           (unsigned int) (numBlocksToWrite >> 20), numGroups,
           numBlocksPerGroup >> 20);
  fill("Filling", indexSession, numGroups, numBlocksPerGroup, &newData, &state,
       cbStatus);

  // Test steady state performance with no dedupe (64M blocks in 8M groups)
  numBlocksToWrite  = 1 << 26;
  numBlocksPerGroup = 1 << 23;
  numGroups = (unsigned int) (numBlocksToWrite / numBlocksPerGroup);
  albPrint("Add %uM steady state chunks (no dedupe)",
           (unsigned int) (numBlocksToWrite >> 20));
  fill("Steady", indexSession, numGroups, numBlocksPerGroup, &newData, &state,
       cbStatus);

  // Test steady state performance with 50% dedupe (64M blocks in 8M groups)
  numBlocksToWrite  = 1 << 26;
  numBlocksPerGroup = 1 << 23;
  numGroups = (unsigned int) (numBlocksToWrite / numBlocksPerGroup);
  albPrint("Add %uM steady state chunks with dedupe",
           (unsigned int) (numBlocksToWrite >> 20));
  fill("Dedupe (50%)", indexSession, numGroups, numBlocksPerGroup, &halfDedupe,
       &state, cbStatus);

  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "post block name performance", pbnPerfTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                   = "PostBlockName_p1",
  .initializerWithSession = initializerWithSession,
  .tests                  = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
