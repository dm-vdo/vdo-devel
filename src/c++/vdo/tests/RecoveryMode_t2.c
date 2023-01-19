/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "block-allocator.h"
#include "physical-zone.h"
#include "slab.h"
#include "slab-depot.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "recoveryModeUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  PHYSICAL_THREAD_COUNT = 4,
};

static bool          stillInRecovery;
static zone_count_t  expectedScrubberWaitingZone;

static block_count_t dataBlocksPerSlab;

/**
 * Test-specific initialization.
 **/
static void initializeRecoveryModeT2(void)
{
  const TestParameters parameters = {
    .logicalThreadCount  = 1,
    .physicalThreadCount = PHYSICAL_THREAD_COUNT,
    .hashZoneThreadCount = 1,
    .journalBlocks       = 32,
    .slabCount           = 4,
    .slabJournalBlocks   = 8,
    .slabSize            = 32,
    .logicalBlocks       = 12500,
    // Geometry + super block + root count + four slabs + recovery journal
    // + slab summary
    .physicalBlocks      = 1 + 1 + 60 + (32 * 4) + 32 + 64,
  };
  initializeRecoveryModeTest(&parameters);
  stillInRecovery = false;
  dataBlocksPerSlab = vdo->depot->slab_config.data_blocks;

  // Initialize all the important parts of the block map tree. There is no
  // space thereafter.
  populateBlockMapTree();

  // We want exactly 8 slabs for data, two slabs per physical zone.
  addSlabs(PHYSICAL_THREAD_COUNT * 2);

  // The resume which happened in addSlabs() reordered the priority table.
  // Restarting the VDO restores the ordering the test depends upon.
  restartVDO(false);

}

/**********************************************************************/
static void checkVDORecovery(struct vdo_completion *completion)
{
  stillInRecovery = vdo_in_recovery_mode(vdo);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Get the LBN which will be the first logical block written
 * to this slab in this test.
 **/
static logical_block_number_t
getLogicalBlockStartForSlab(slab_count_t slabNumber)
{
  return (slabNumber * dataBlocksPerSlab);
}

/**
 * Get the offset which will be the data written to the first block in
 * this slab in this test.
 **/
static block_count_t getDataOffsetStartForSlab(slab_count_t  slabNumber,
                                               block_count_t blocksPerSlab)
{
  return ((slabNumber * blocksPerSlab) + 1); // Always skip the 0 block.
}

/**********************************************************************/
static void setNextAllocationZone(zone_count_t targetSlabZone)
{
  // Only one logical thread in this test.
  struct logical_zone *zone = &vdo->logical_zones->zones[0];
  zone->allocation_zone  = &vdo->physical_zones->zones[targetSlabZone];
  zone->allocation_count = 0;
}

/**
 * Test that recovery with some zones with only clean slabs still
 * recovers successfully.
 **/
static void testMultipleZoneCleanZoneRecovery(void)
{
  writeData(0, 1, 1, VDO_SUCCESS);
  crashVDO();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  verifyData(0, 1, 1);
}

/**
 * Implements LockedMethod.
 **/
static bool setWaitingZone(void *context)
{
  expectedScrubberWaitingZone
    = ((struct physical_zone *) context)->zone_number;
  return true;
}

/**
 * Make assertions about the vio which must now be waiting.
 *
 * <p>Implements VDOAction.
 **/
static void ensureWaitingToScrub(struct vdo_completion *completion)
{
  if (runSavedCallback(completion)) {
    return;
  }

  struct data_vio *dataVIO = as_data_vio(completion);
  CU_ASSERT(dataVIO->allocation.wait_for_clean_slab);
  runLocked(setWaitingZone, dataVIO->allocation.zone);
}

/**
 * Check that a vio is a data VIO waiting for a clean slab on the expected
 * thread.
 *
 * Implements CompletionHook.
 **/
static bool wrapIfVIOAboutToWait(struct vdo_completion *completion)
{
  if (!isDataVIO(completion)) {
    return true;
  }

  struct allocation *allocation = &(as_data_vio(completion)->allocation);
  if (allocation->wait_for_clean_slab
      || ((allocation->zone != NULL)
          && (allocation->zone->next->zone_number
              == allocation->first_allocation_zone))) {
    wrapCompletionCallback(completion, ensureWaitingToScrub);
  }

  return true;
}

/**
 * Implements WaitCondition.
 **/
static bool checkVIOWaitingToScrub(void *context)
{
  return (*((zone_count_t *) context) < PHYSICAL_THREAD_COUNT);
}

/**
 * Test that space in scrubbed slab in later zone is used before space
 * in unscrubbed slab in current zone.
 **/
static void testMultipleZoneSomeSpaceRecovery(void)
{
  // Four slabs are devoted to the block map.
  slab_count_t            slabCount = vdo->depot->slab_count - 4;
  logical_block_number_t  nextLBN;
  block_count_t           dataOffset;

  setNextAllocationZone(0); // start at the beginning

  block_count_t blockCount = getPhysicalBlocksFree();

  struct vdo_slab *newSlab;
  zone_count_t     slabZones[slabCount];
  slab_count_t     slabNumbers[slabCount];

  for (slab_count_t i = 0; i < slabCount; i++) {
    nextLBN = getLogicalBlockStartForSlab(i);
    dataOffset = getDataOffsetStartForSlab(i, dataBlocksPerSlab);
    VDO_ASSERT_SUCCESS(performIndexedWrite(nextLBN, 1, dataOffset));
    newSlab = vdo_get_slab(vdo->depot, lookupLBN(nextLBN).pbn);
    slabZones[i] = newSlab->allocator->zone_number;
    // We require the slabs are handed out, two from each zone, before moving
    // to a new zone.
    CU_ASSERT_EQUAL(slabZones[i], i / 2);
    slabNumbers[i] = newSlab->slab_number;

    // Fill the rest of this slab.
    nextLBN++;
    dataOffset++;
    writeData(nextLBN, dataOffset, dataBlocksPerSlab - 1, VDO_SUCCESS);
  }

  // Trim open a block in all slabs.
  for (slab_count_t i = 0; i < slabCount; i++) {
    nextLBN = getLogicalBlockStartForSlab(i);
    discardData(nextLBN, 1, VDO_SUCCESS);
  }

  // Crash then restart.
  crashVDO();
  // Latch all data slabs.
  for (slab_count_t i = 0; i < slabCount; i += 1) {
     setupSlabScrubbingLatch(slabNumbers[i]);
  }
  startVDO(VDO_DIRTY);
  // Wait for the first ones to be latched.
  for (slab_count_t i = 0; i < slabCount; i += 2) {
    waitForSlabLatch(slabNumbers[i]);
  }

  nextLBN    = blockCount;
  dataOffset = blockCount + 1;
  for (slab_count_t i = 0; i < slabCount; i += 2) {
    // Release the latch on the next slab to use.
    releaseSlabLatch(slabNumbers[i]);
    // Wait for the next slab latch in this zone, implying the latched slab
    // is through scrubbing.
    waitForSlabLatch(slabNumbers[i + 1]);
    setNextAllocationZone(0); // start at the beginning
    VDO_ASSERT_SUCCESS(performIndexedWrite(nextLBN, 1, dataOffset));

    // Confirm that the new block is not in the target zone, but spilled
    // over to a zone with space.
    newSlab = vdo_get_slab(vdo->depot, lookupLBN(nextLBN).pbn);
    CU_ASSERT_EQUAL(newSlab->allocator->zone_number, slabZones[i]);
    nextLBN++;
    dataOffset++;
  }

  setCompletionEnqueueHook(wrapIfVIOAboutToWait);
  for (slab_count_t i = 1; i < slabCount; i += 2) {
    setNextAllocationZone(0); // start at the beginning
    expectedScrubberWaitingZone = PHYSICAL_THREAD_COUNT;
    IORequest *request = launchIndexedWrite(nextLBN, 1, dataOffset);
    waitForCondition(checkVIOWaitingToScrub, &expectedScrubberWaitingZone);
    slab_count_t slabToUse
      = slabNumbers[(expectedScrubberWaitingZone * 2) + 1];
    releaseSlabLatch(slabToUse);
    awaitAndFreeSuccessfulRequest(request);
    newSlab = vdo_get_slab(vdo->depot, lookupLBN(nextLBN).pbn);
    CU_ASSERT_EQUAL(newSlab->allocator->zone_number,
                    expectedScrubberWaitingZone);
    nextLBN++;
    dataOffset++;
  }
}

/**
 * Implements WaitCondition.
 **/
static bool checkVIOWaitingToScrubInZone3(void *context)
{
  return (*((zone_count_t *) context) == 3);
}

/**
 * Test that the VDO_NO_SPACE error waits until recovery complete.
 **/
static void testMultipleZoneNoSpaceRecovery(void)
{
  // Unique data write to fill the physical space.
  logical_block_number_t nextLBN    = 0;
  block_count_t          dataOffset = 1;
  block_count_t          blockCount = getPhysicalBlocksFree();
  writeAndVerifyData(nextLBN, dataOffset, blockCount, 0, blockCount);
  nextLBN    += blockCount;
  dataOffset += blockCount;

  // We assume this will be in zone 3.
  slab_count_t targetSlabIndex = vdo->depot->slab_count - 1;
  // Crash then restart.
  crashVDO();
  setupSlabScrubbingLatch(targetSlabIndex);
  startVDO(VDO_DIRTY);
  waitForSlabLatch(targetSlabIndex);

  // Attempt to write a unique block.  Get VDO_NO_SPACE only when out of
  // recovery mode.
  expectedScrubberWaitingZone = PHYSICAL_THREAD_COUNT;
  setCompletionEnqueueHook(wrapIfVIOAboutToWait);
  IORequest *lateWrite
    = launchIndexedWrite(nextLBN, 1, dataOffset);
  performSuccessfulAction(checkVDORecovery);
  CU_ASSERT_TRUE(stillInRecovery);
  waitForCondition(checkVIOWaitingToScrubInZone3,
                   &expectedScrubberWaitingZone);
  clearCompletionEnqueueHooks();
  releaseSlabLatch(targetSlabIndex);
  CU_ASSERT_EQUAL(VDO_NO_SPACE, awaitAndFreeRequest(UDS_FORGET(lateWrite)));
  performSuccessfulAction(checkVDORecovery);
  CU_ASSERT_FALSE(stillInRecovery);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Recover with clean zone",  testMultipleZoneCleanZoneRecovery },
  { "Find space in new zone",   testMultipleZoneSomeSpaceRecovery },
  { "Fail write when VDO full", testMultipleZoneNoSpaceRecovery   },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "VDO recovery mode tests (RecoveryMode_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeRecoveryModeT2,
  .cleaner                  = tearDownRecoveryModeTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
