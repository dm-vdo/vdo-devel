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

#include "block-map.h"
#include "encodings.h"
#include "slab-depot.h"

#include "vdoConfig.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  TEST_BLOCKS = 60,
  MAX_TRIES   = 3,
};

static block_count_t           expectedLogicalBlocksUsed;
static block_count_t           packedVIOs = 0;
static physical_block_number_t pbn;
static bool                    logicalThreadVisited;
static bool                    blockMapDrained;
static bool                    crashOnSlabDepotLoad;

typedef struct refCountData {
  /** The number of reference counters in the array */
  uint32_t        counterCount;
  /** reference count array */
  vdo_refcount_t *counters;
} RefCountData;

typedef struct preRebuildData {
  /** The number of expected free blocks in the original slab depot */
  block_count_t expectedFreeBlocks;
  /** The slab count of the original slab depot */
  slab_count_t  slabCount;
  /** The slabs' original reference counts */
  RefCountData *refCountData;
} PreRebuildData;

/**
 * Initialize the index, vdo, and test data.
 **/
static void initializeRebuildT1(void)
{
  TestParameters parameters = {
    .logicalBlocks       = TEST_BLOCKS * TEST_BLOCKS * 2,
    .mappableBlocks      = TEST_BLOCKS + TEST_BLOCKS,
    .slabSize            = 16,
    .slabJournalBlocks   = 4,
    .journalBlocks       = 32,
    .physicalThreadCount = 1,
  };
  initializeVDOTest(&parameters);

  expectedLogicalBlocksUsed = 0;
}

/**
 * Implements VDOAction.
 **/
static void signalPackedVIO(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  packedVIOs++;
  broadcast();
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfLeavingCompressor(struct vdo_completion *completion)
{
  if (isLeavingCompressor(completion)) {
    wrapCompletionCallback(completion, signalPackedVIO);
  }

  return true;
}

/**
 * Implements WaitCondition.
 **/
static bool checkPackedVIOCount(void *context __attribute__((unused)))
{
  return (packedVIOs >= (TEST_BLOCKS - 1));
}

/**
 * Implements LockedMethod.
 **/
static bool assertPackedVIOCount(void *context __attribute__((unused)))
{
  CU_ASSERT_EQUAL(packedVIOs, TEST_BLOCKS - 1);
  packedVIOs = 0;
  return false;
}

/**
 * Write a sparse pattern of test data to the VDO, leaving gaps to
 * ensure that some block map pages are not touched.
 **/
static void writeTestData(logical_block_number_t startBlock,
                          block_count_t          dataOffset,
                          bool                   compress)
{
  block_count_t writeLength;
  if (!compress) {
    // With no compression, we can just write the test data.
    for (block_count_t i = 0; i < TEST_BLOCKS; i += 3) {
      writeLength = TEST_BLOCKS - i;
      expectedLogicalBlocksUsed += writeLength;
      writeData(startBlock + (i * TEST_BLOCKS), dataOffset + i, writeLength,
                VDO_SUCCESS);
    }
    return;
  }

  // If compression is active, the initial VIOs will go through the packer.
  setCompletionEnqueueHook(wrapIfLeavingCompressor);
  writeLength = TEST_BLOCKS;
  expectedLogicalBlocksUsed += writeLength;
  IORequest *request
    = launchIndexedWrite(startBlock, writeLength, dataOffset);

  // Wait for all VIOs to get to the packer.
  runOnCondition(checkPackedVIOCount, assertPackedVIOCount, NULL);

  // Flush VIOs out of the packer and wait for the request to finish.
  requestFlushPacker();

  awaitAndFreeSuccessfulRequest(vdo_forget(request));
  clearCompletionEnqueueHooks();

  // Issue more writes which will all deduplicate.
  for (block_count_t i = 3; i < TEST_BLOCKS; i += 3) {
    writeLength = TEST_BLOCKS - i;
    expectedLogicalBlocksUsed += writeLength;
    writeData(startBlock + (i * TEST_BLOCKS), dataOffset + i, writeLength,
              VDO_SUCCESS);
  }
}

/**
 * Write the data and wait for the VDO statistics to stabilize.
 **/
static void writeInitialTestData(bool compress)
{
  writeTestData(0, 0, compress);

  int tries = 0;
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  while ((tries < MAX_TRIES)
         && (stats.logical_blocks_used != expectedLogicalBlocksUsed)) {
    tries++;
    sleep(1);
    vdo_fetch_statistics(vdo, &stats);
  }

  CU_ASSERT_EQUAL(stats.logical_blocks_used, expectedLogicalBlocksUsed);
}

/**
 * Verify data on the VDO against the pattern written by writeTestData().
 **/
static void verifyTestData(logical_block_number_t startBlock,
                           block_count_t          dataOffset)
{
  for (block_count_t i = 0; i < TEST_BLOCKS; i += 3) {
    verifyData(startBlock + (i * TEST_BLOCKS), dataOffset + i,
               (TEST_BLOCKS - i));
  }
}

/**
 * Verify the reference counts after rebuild match what was in memory
 * before the crash.
 *
 * @param originalRefCountData  The size and array of counters
 **/
static void verifyRefCountData(RefCountData *originalRefCountData)
{
  struct slab_depot *currentDepot = vdo->depot;
  for (slab_count_t s = 0; s < currentDepot->slab_count; s++) {
    struct vdo_slab *slab = currentDepot->slabs[s];
    RefCountData original = originalRefCountData[s];
    CU_ASSERT_EQUAL(original.counterCount, slab->block_count);
    for (block_count_t block = 0; block < slab->block_count; block++) {
      vdo_refcount_t oldCount = original.counters[block];
      vdo_refcount_t newCount = slab->counters[block];
      if (oldCount == newCount) {
        continue;
      }

      if ((oldCount == 254) && (newCount == 1)) {
        continue;
      }

      CU_FAIL("Reference count mismatch slab %u, block %llu was %u, is %u",
              s, (unsigned long long) block, oldCount, newCount);
    }
  }
}

/**********************************************************************/
static void doAllocateBlock(struct vdo_completion *completion)
{
  struct slab_depot *depot = vdo->depot;
  VDO_ASSERT_SUCCESS(vdo_allocate_block(&depot->allocators[0], &pbn));
  struct reference_updater updater = {
    .operation = VDO_JOURNAL_DATA_REMAPPING,
    .increment = true,
    .zpbn = {
      .pbn = pbn,
    },
  };
  VDO_ASSERT_SUCCESS(adjust_reference_count(vdo_get_slab(depot, pbn), &updater, NULL));
  vdo_finish_completion(completion);
}

/**********************************************************************/
static void doDecrementReferenceCount(struct vdo_completion *completion)
{
  struct slab_depot *depot = vdo->depot;
  struct reference_updater updater = {
    .operation = VDO_JOURNAL_DATA_REMAPPING,
    .increment = false,
    .zpbn = {
      .pbn = pbn,
    },
  };
  VDO_ASSERT_SUCCESS(adjust_reference_count(vdo_get_slab(depot, pbn), &updater, NULL));
  vdo_finish_completion(completion);
}

/**********************************************************************/
static void verifyRebuiltDepot(PreRebuildData *originalData)
{
  // Rebuilt depot must have the same free blocks as original.
  block_count_t expectedFreeBlocks = getPhysicalBlocksFree();
  CU_ASSERT_EQUAL(expectedFreeBlocks, originalData->expectedFreeBlocks);

  // The rebuilt depot can allocate its free blocks.
  physical_block_number_t actualAllocations[expectedFreeBlocks];
  for (block_count_t i = 0; i < expectedFreeBlocks; i++) {
    performSuccessfulAction(doAllocateBlock);
    actualAllocations[i] = pbn;
  }
  CU_ASSERT_EQUAL(0, getPhysicalBlocksFree());

  // Free all the newly allocated blocks so we can reuse the depot.
  for (block_count_t i = 0; i < expectedFreeBlocks; i++) {
    pbn = actualAllocations[i];
    performSuccessfulAction(doDecrementReferenceCount);
  }
  CU_ASSERT_EQUAL(expectedFreeBlocks, getPhysicalBlocksFree());
}

/**
 * Copy a depot's slabs' refCount data.
 *
 * @param allocator  The block allocator whose data to copy
 *
 * @return a copy of the data in an array for each slab
 **/
static PreRebuildData *copyPreRebuildData(struct slab_depot *depot)
{
  PreRebuildData *originalData;
  VDO_ASSERT_SUCCESS(vdo_allocate(1, PreRebuildData, __func__, &originalData));
  originalData->expectedFreeBlocks = getPhysicalBlocksFree();
  originalData->slabCount = depot->slab_count;
  VDO_ASSERT_SUCCESS(vdo_allocate(depot->slab_count, RefCountData, __func__,
                                  &originalData->refCountData));
  for (size_t i = 0; i < depot->slab_count; i++) {
    struct vdo_slab *slab              = depot->slabs[i];
    RefCountData    *originalRefCounts = &originalData->refCountData[i];
    originalRefCounts->counterCount    = slab->block_count;
    VDO_ASSERT_SUCCESS(vdo_allocate(slab->block_count,
                                    vdo_refcount_t,
                                    __func__,
                                    &(originalRefCounts->counters)));
    memcpy(originalRefCounts->counters,
           slab->counters,
           slab->block_count * sizeof(vdo_refcount_t));
  }

  return originalData;
}

/**********************************************************************/
static void freePreRebuildData(PreRebuildData **originalDataPtr)
{
  PreRebuildData *originalData = *originalDataPtr;

  for (size_t i = 0; i < originalData->slabCount; i++) {
    vdo_free(vdo_forget(originalData->refCountData[i].counters));
  }

  vdo_free(originalData->refCountData);
  originalData->refCountData = NULL;
  vdo_free(originalData);
  *originalDataPtr = NULL;
}

/**********************************************************************/
static void prepareForRebuildTest(PreRebuildData       **originalData,
                                  struct vdo_statistics *originalStats,
                                  bool                   compress)
{
  writeInitialTestData(compress);
  *originalData = copyPreRebuildData(vdo->depot);
  vdo_fetch_statistics(vdo, originalStats);
}

/**********************************************************************/
static void
rebuildAndVerify(PreRebuildData        **originalDataPtr,
                 struct vdo_statistics  *originalStats,
                 enum vdo_state          expectedState,
                 uint64_t                expectedCompleteRecoveries,
                 uint64_t                expectedReadOnlyRecoveries)
{
  // Rebuild.
  setStartStopExpectation(VDO_SUCCESS);
  startVDO(expectedState);
  waitForRecoveryDone();

  verifyRefCountData((*originalDataPtr)->refCountData);
  verifyTestData(0, 0);
  verifyRebuiltDepot(*originalDataPtr);

  freePreRebuildData(originalDataPtr);

  struct vdo_statistics rebuiltStats;
  vdo_fetch_statistics(vdo, &rebuiltStats);
  CU_ASSERT_NOT_EQUAL(0, originalStats->logical_blocks_used);
  CU_ASSERT_EQUAL(originalStats->logical_blocks_used,
                  rebuiltStats.logical_blocks_used);
  CU_ASSERT_EQUAL(originalStats->data_blocks_used,
                  rebuiltStats.data_blocks_used);
  CU_ASSERT_EQUAL(originalStats->overhead_blocks_used,
                  rebuiltStats.overhead_blocks_used);
  CU_ASSERT_EQUAL(expectedCompleteRecoveries,
                  vdo->states.vdo.complete_recoveries);
  CU_ASSERT_EQUAL(expectedReadOnlyRecoveries,
                  vdo->states.vdo.read_only_recoveries);
}

/**********************************************************************/
static void testRebuildTwice(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  prepareForRebuildTest(&originalData, &originalStats, false);
  crashVDO();
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 1, 0);

  // Do another rebuild to verify that we can handle another failure.
  originalData = copyPreRebuildData(vdo->depot);
  crashVDO();
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 2, 0);
}

/**********************************************************************/
static void testForceRebuild(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  prepareForRebuildTest(&originalData, &originalStats, false);
  forceRebuild();
  rebuildAndVerify(&originalData, &originalStats, VDO_FORCE_REBUILD, 1, 1);

  // Check that after we've rebuilt, the super block is in fact clean.
  stopVDO();
  checkVDOState(VDO_CLEAN);
}

/**********************************************************************/
static void testRebuildWithCompressedBlocks(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  modifyCompressDedupe(true, true);
  prepareForRebuildTest(&originalData, &originalStats, true);
  crashVDO();
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 1, 0);
}

/**********************************************************************/
static void testRebuildAfterResize(void)
{
  writeInitialTestData(false);

  // Resize the VDO.
  TestConfiguration testConfig = getTestConfig();
  block_count_t newSize = testConfig.config.physical_blocks;
  newSize = (newSize * 2) - testConfig.vdoRegionStart + 1;
  growVDOPhysical(newSize, VDO_SUCCESS);

  // Write some additional data after the resize.
  logical_block_number_t startBlock = TEST_BLOCKS * TEST_BLOCKS;
  writeTestData(startBlock, TEST_BLOCKS, false);

  PreRebuildData *originalData = copyPreRebuildData(vdo->depot);
  struct vdo_statistics originalStats;
  vdo_fetch_statistics(vdo, &originalStats);

  crashVDO();
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 1, 0);
  // Verify the second data set.
  verifyTestData(startBlock, TEST_BLOCKS);
}

/**********************************************************************/
static void setRequeueAndRun(struct vdo_completion *completion)
{
  completion->requeue = true;
  runSavedCallback(completion);
}

/**
 * Fails slab depot load.
 *
 * Implements CompletionHook.
 **/
static bool failSlabDepotLoad(struct vdo_completion *completion)
{
  if (completion->type != VDO_REPAIR_COMPLETION) {
    return true;
  }

  if (!logicalThreadVisited) {
    if (completion->callback_thread_id == vdo->thread_config.logical_threads[0]) {
      logicalThreadVisited = true;
    }

    return true;
  }

  if (completion->callback_thread_id == vdo->thread_config.admin_thread) {
    if (!blockMapDrained) {
      /*
       * We need to wrap flush_block_map() so that we can set the requeue flag
       * on the recovery completion so that this hook gets to fire again when
       * flush_block_map() is done.
       */
      wrapCompletionCallback(completion, setRequeueAndRun);
      blockMapDrained = true;
      return true;
    }

    if (crashOnSlabDepotLoad) {
      flushRAMLayer(getSynchronousLayer());
      prepareToCrashRAMLayer(getSynchronousLayer());
    } else {
      vdo_set_completion_result(completion, BLK_STS_VDO_INJECTED);
    }

    // Turn off this hook, and prevent all further writes.
    removeCompletionEnqueueHook(failSlabDepotLoad);
  }

  return true;
}

/**
 * Test crashing during recovery after the block map is rebuilt, but before
 * recovering the reference counts.
 **/
static void testCrashBeforeRefCountRebuild(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  prepareForRebuildTest(&originalData, &originalStats, false);
  crashVDO();

  // Set a hook to crash the vdo before loading the slab depot.
  logicalThreadVisited = false;
  blockMapDrained = false;
  crashOnSlabDepotLoad = true;
  setCompletionEnqueueHook(failSlabDepotLoad);
  startVDO(VDO_DIRTY);
  stopVDO();
  crashRAMLayer(getSynchronousLayer());

  // Let the vdo recover.
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 1, 0);
}

/**
 * Test an error during recovery after the block map is rebuilt, but before
 * recovering the reference counts.
 **/
static void testErrorBeforeRefCountRebuild(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  prepareForRebuildTest(&originalData, &originalStats, false);
  crashVDO();

  // Set a hook to inject an error on loading the slab depot.
  logicalThreadVisited = false;
  blockMapDrained = false;
  crashOnSlabDepotLoad = false;
  setCompletionEnqueueHook(failSlabDepotLoad);
  startReadOnlyVDO(VDO_DIRTY);
  stopVDO();

  // Rebuild the vdo.
  VDO_ASSERT_SUCCESS(forceVDORebuild(getSynchronousLayer()));
  setStartStopExpectation(VDO_SUCCESS);
  rebuildAndVerify(&originalData, &originalStats, VDO_FORCE_REBUILD, 1, 1);
}

/**
 * Check whether a completion is a super block write
 **/
static bool isSuperBlockWrite(struct vdo_completion *completion)
{
  return (vioTypeIs(completion, VIO_TYPE_SUPER_BLOCK) && isMetadataWrite(completion));
}

/**
 * Fails the super block write before it is written in the RAMLayer.
 *
 * Implements BIOSubmitHook.
 **/
static bool failBeforeSuperBlockWrite(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if (!isSuperBlockWrite(&vio->completion)) {
    return true;
  }

  // Set a bad error code to force a failed write.
  clearBIOSubmitHook();
  flushRAMLayer(getSynchronousLayer());
  prepareToCrashRAMLayer(getSynchronousLayer());
  vdo_set_completion_result(&vio->completion, BLK_STS_VDO_INJECTED);
  bio->bi_end_io(bio);
  return false;
}

/**
 * Test failing during load after recovery but before saving the super block.
 **/
static void testCrashAfterRecovery(void)
{
  PreRebuildData       *originalData;
  struct vdo_statistics originalStats;
  prepareForRebuildTest(&originalData, &originalStats, false);
  crashVDO();

  // Set hook and VDO load will fail before the super block is written.
  setBIOSubmitHook(failBeforeSuperBlockWrite);
  crashRAMLayer(getSynchronousLayer());
  startReadOnlyVDO(VDO_DIRTY);
  stopVDO();

  // Let the VDO rebuild.
  rebuildAndVerify(&originalData, &originalStats, VDO_DIRTY, 1, 0);
}

/**
 * Fails any block map page read during the block map rebuild.
 *
 * Implements BIOSubmitHook.
 **/
static bool failDuringBlockMapRead(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((vio->type != VIO_TYPE_BLOCK_MAP) || (bio_op(bio) != REQ_OP_READ)) {
    return true;
  }

  clearBIOSubmitHook();
  bio->bi_status = BLK_STS_VDO_INJECTED;
  bio->bi_end_io(bio);
  return false;
}

/**
 * Test a block map page read error during read-only rebuild.
 **/
static void testBlockMapReadError(void)
{
  writeInitialTestData(false);
  crashVDO();

  // Set hook and VDO load will fail during the first block map read.
  setBIOSubmitHook(failDuringBlockMapRead);
  startReadOnlyVDO(VDO_DIRTY);
  stopVDO();

  // Set the hook again to keep the same error, but the read-only rebuild
  // should succeed.
  VDO_ASSERT_SUCCESS(forceVDORebuild(getSynchronousLayer()));
  setBIOSubmitHook(failDuringBlockMapRead);
  setStartStopExpectation(VDO_SUCCESS);
  startVDO(VDO_FORCE_REBUILD);

  // Check that after we've rebuilt, the super block is in fact clean.
  stopVDO();
  checkVDOState(VDO_CLEAN);
}

/**
 * Corrupt the VIO which was reading a block map page by changing
 * the VIO's data to look like a block map page with an invalid PBN.
 *
 * Implements CompletionHook.
 **/
static bool corruptVIO(struct vdo_completion *completion)
{
  if ((!vioTypeIs(completion, VIO_TYPE_BLOCK_MAP))
      || !isMetadataRead(completion)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  vdo_format_block_map_page(vio->data,
                            vdo->states.vdo.nonce,
                            0,
                            true);
  removeCompletionEnqueueHook(corruptVIO);
  vio->bio->bi_end_io(vio->bio);
  return false;
}

/**
 * Test a block map page read-hook error during read-only rebuild.
 **/
static void testBlockMapBadPageError(void)
{
  writeInitialTestData(false);
  crashVDO();

  // Set hook and VDO load will fail during the first block map read.
  setCompletionEnqueueHook(corruptVIO);
  startReadOnlyVDO(VDO_DIRTY);
  stopVDO();

  // Set the hook again to keep the same error, but the read-only rebuild
  // should succeed.
  VDO_ASSERT_SUCCESS(forceVDORebuild(getSynchronousLayer()));
  setCompletionEnqueueHook(corruptVIO);
  setStartStopExpectation(VDO_SUCCESS);
  startVDO(VDO_FORCE_REBUILD);

  // Check that after we've rebuilt, the super block is in fact clean.
  stopVDO();
  checkVDOState(VDO_CLEAN);
}

/**********************************************************************/
static CU_TestInfo vdoTests[] = {
  { "rebuild VDO twice",                     testRebuildTwice                },
  { "rebuild VDO with compressed blocks",    testRebuildWithCompressedBlocks },
  { "rebuild VDO after resize",              testRebuildAfterResize          },
  { "force rebuild for a read-only VDO",     testForceRebuild                },
  { "crash before ref count rebuild",        testCrashBeforeRefCountRebuild  },
  { "error before ref count rebuild",        testErrorBeforeRefCountRebuild  },
  { "crash after recovery",                  testCrashAfterRecovery          },
  { "read error during block map rebuild",   testBlockMapReadError           },
  { "invalid page during block map rebuild", testBlockMapBadPageError        },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "Rebuild VDO tests (Rebuild_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeRebuildT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
