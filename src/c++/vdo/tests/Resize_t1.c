/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "slab-depot.h"
#include "thread-config.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "recoveryModeUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  DATA_BLOCKS   = 64,
  GROWTH_AMOUNT = 128,
};

/**
 * Test-specific initialization.
 **/
static void initializeResizeT1(void)
{
  TestParameters parameters = {
    .mappableBlocks    = 64,
    .logicalBlocks     = 256,
    .journalBlocks     = 8,
    .slabJournalBlocks = 4,
    .slabSize          = 16,
    .dataFormatter     = fillWithOffsetPlusOne,
  };
  initializeRecoveryModeTest(&parameters);
}

/**
 * Restart VDO and validate the state as recorded in the super block.
 **/
static void validateSuperBlock(void)
{
  struct vdo_statistics statsBefore;
  vdo_fetch_statistics(vdo, &statsBefore);
  block_count_t overheadBefore = vdo_get_physical_blocks_overhead(vdo);

  restartVDO(false);
  struct vdo_statistics statsAfter;
  vdo_fetch_statistics(vdo, &statsAfter);
  CU_ASSERT_EQUAL(statsBefore.data_blocks_used,
		  statsAfter.data_blocks_used);
  CU_ASSERT_EQUAL(statsBefore.logical_blocks_used,
		  statsAfter.logical_blocks_used);
  CU_ASSERT_EQUAL(overheadBefore, vdo_get_physical_blocks_overhead(vdo));
}

/**********************************************************************/
static bool injectSuperBlockWriteError(struct bio *bio) {
  struct vio *vio = bio->bi_private;
  if (vioTypeIs(&vio->completion, VIO_TYPE_SUPER_BLOCK)) {
    setVIOResult(vio, -EROFS);
    clearBIOSubmitHook();
    bio->bi_end_io(bio);
    return false;
  }

  return true;
}

/**********************************************************************/
static void testAddStorageWithWriteError(void)
{
  block_count_t  physicalBlocks = getTestConfig().config.physical_blocks;
  block_count_t  dataBlocks     = computeDataBlocksToFill();
  block_count_t  blocksToWrite  = dataBlocks / 2;
  writeAndVerifyData(0, 0, blocksToWrite, dataBlocks - blocksToWrite,
                     blocksToWrite);

  setBIOSubmitHook(injectSuperBlockWriteError);
  growVDOPhysical(physicalBlocks + GROWTH_AMOUNT, -EROFS);

  // The VDO should be suspended and read-only in memory, but not on disk.
  verifyReadOnly();
  CU_ASSERT(vdo_get_admin_state(vdo)->quiescent);

  // So if we start it again, it should not be read-only.
  setStartStopExpectation(VDO_SUCCESS);
  // We can't use restartVDO() here because it copies the config even though
  // the config is for the failed growth.
  stopAsyncLayer();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  writeAndVerifyData(0,
                     blocksToWrite,
                     blocksToWrite,
                     dataBlocks - blocksToWrite,
                     blocksToWrite);

  /*
   * Now do the failed grow again and confirm that even if we follow it with
   * what would be a successful grow, it neither grows nor becomes read-only
   * on disk.
   */
  setBIOSubmitHook(injectSuperBlockWriteError);
  growVDOPhysical(physicalBlocks + GROWTH_AMOUNT, -EROFS);
  growVDOPhysical(physicalBlocks + GROWTH_AMOUNT, VDO_READ_ONLY);
  verifyReadOnly();
  // We can't use restartVDO() here because we need to reset the start
  // expectation between the stop and start.
  stopAsyncLayer();
  setStartStopExpectation(VDO_SUCCESS);
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  // Attempt to overwrite existing data. Do it in two chunks so that we don't
  // have a transient out-of-space error.
  block_count_t halfBlocksToWrite = blocksToWrite / 2;
  writeAndVerifyData(0,
                     0,
                     halfBlocksToWrite,
                     dataBlocks - blocksToWrite,
                     blocksToWrite);
  writeAndVerifyData(halfBlocksToWrite,
                     halfBlocksToWrite,
                     halfBlocksToWrite,
                     dataBlocks - blocksToWrite,
                     blocksToWrite);
}

/**********************************************************************/
static void testAddStorage(void)
{
  struct vdo_config config         = getTestConfig().config;
  block_count_t     physicalBlocks = config.physical_blocks;
  slab_count_t      slabCount      = vdo->depot->slab_count;

  // Fill the physical space.
  block_count_t dataBlocks       = fillPhysicalSpace(0, 0);
  block_count_t expectedOverhead = physicalBlocks - dataBlocks;
  CU_ASSERT_EQUAL(expectedOverhead, vdo_get_physical_blocks_overhead(vdo));

  // Verify that the physical space is full.
  writeData(dataBlocks + 5, dataBlocks + 5, 1, VDO_NO_SPACE);

  // Test setting VDO physical storage to be the same as it is.
  growVDOPhysical(physicalBlocks, VDO_SUCCESS);

  // Verify that the physical space is still full.
  writeData(dataBlocks + 5, dataBlocks + 5, 1, VDO_NO_SPACE);

  // Test trying to shrink VDO physical storage, should fail.
  growVDOPhysical(physicalBlocks - 1, -EINVAL);

  struct vdo_statistics statsBefore;
  vdo_fetch_statistics(vdo, &statsBefore);
  CU_ASSERT_EQUAL(statsBefore.physical_blocks, physicalBlocks);

  // Store what we assert is the current journal region.
  config = getTestConfig().config;
  struct partition *partition = vdo_get_known_partition(&vdo->layout,
                                                        VDO_RECOVERY_JOURNAL_PARTITION);
  block_count_t journalBlocks = config.recovery_journal_size;
  physical_block_number_t journalStart
    = (physicalBlocks - journalBlocks - VDO_SLAB_SUMMARY_BLOCKS);
  CU_ASSERT_EQUAL(journalStart, partition->offset);
  size_t journalSize = journalBlocks * VDO_BLOCK_SIZE;
  char buffer[journalSize];
  VDO_ASSERT_SUCCESS(layer->reader(layer, journalStart, journalBlocks, buffer));

  // Grow the underlying storage pool and then expand VDO into it.
  block_count_t newSize = physicalBlocks + GROWTH_AMOUNT;
  growVDOPhysical(newSize, VDO_SUCCESS);

  struct vdo_statistics statsAfter;
  vdo_fetch_statistics(vdo, &statsAfter);
  CU_ASSERT_EQUAL(newSize, statsAfter.physical_blocks);
  CU_ASSERT_EQUAL(statsBefore.logical_blocks, statsAfter.logical_blocks);
  CU_ASSERT_EQUAL(statsBefore.data_blocks_used,
		  statsAfter.data_blocks_used);
  CU_ASSERT_EQUAL(statsBefore.logical_blocks_used,
		  statsAfter.logical_blocks_used);

  config = getTestConfig().config;
  size_t newSlabCount = slabCount + (GROWTH_AMOUNT / config.slab_size);
  CU_ASSERT_EQUAL(newSlabCount, vdo->depot->slab_count);
  block_count_t extraDataBlocks
    = ((newSlabCount - slabCount) * vdo->depot->slab_config.data_blocks);
  block_count_t newOverhead
    = expectedOverhead + (GROWTH_AMOUNT - extraDataBlocks);
  CU_ASSERT_EQUAL(newOverhead, vdo_get_physical_blocks_overhead(vdo));

  // Ensure the journal moved and is still the same.
  physical_block_number_t newJournalStart = journalStart + GROWTH_AMOUNT;
  partition = vdo_get_known_partition(&vdo->layout, VDO_RECOVERY_JOURNAL_PARTITION);
  CU_ASSERT_EQUAL(newJournalStart, partition->offset);
  char newBuffer[journalSize];
  VDO_ASSERT_SUCCESS(layer->reader(layer, newJournalStart, journalBlocks, newBuffer));
  UDS_ASSERT_EQUAL_BYTES(buffer, newBuffer, journalSize);

  // Use the new storage.
  writeAndVerifyData(dataBlocks, dataBlocks, extraDataBlocks, 0,
                     dataBlocks + extraDataBlocks);
  validateSuperBlock();
}

/**********************************************************************/
static void testAddStorageInRecoveryMode(void)
{
  // Write an arbitrary amount of data; if we write none, recovery won't occur.
  block_count_t  dataBlocks    = computeDataBlocksToFill();
  block_count_t  blocksToWrite = dataBlocks / 2;
  writeAndVerifyData(0, 0, blocksToWrite, dataBlocks - blocksToWrite,
                     blocksToWrite);

  // Simulate a crash and restart the dirty VDO to enter recovery mode.
  crashVDO();
  setupSlabScrubbingLatch(1);
  startVDO(VDO_DIRTY);
  waitForSlabLatch(1);

  // VDO should be in recovery mode after load finished.
  CU_ASSERT_TRUE(vdo_in_recovery_mode(vdo));

  // Test that an attempt to resize while in recovery mode will fail safely
  // with a clear error.
  block_count_t oldSize = getTestConfig().config.physical_blocks;
  growVDOPhysical(oldSize * 2, vdo_map_to_system_error(VDO_RETRY_AFTER_REBUILD));
  CU_ASSERT_EQUAL(oldSize, getTestConfig().config.physical_blocks);

  // Release the latch and wait until VDO leaves the recovery mode.
  releaseSlabLatch(1);
  waitForRecoveryDone();
  validateSuperBlock();
}

/**********************************************************************/
static void testTooSmallGrowth(void)
{
  // Growing by less than the journal size plus the slab summary size
  // should fail, since they would need to be copied atop each other.
  struct vdo_config config = getTestConfig().config;
  block_count_t metadataSize
    = (config.recovery_journal_size + VDO_SLAB_SUMMARY_BLOCKS);
  CU_ASSERT_TRUE(config.slab_size < (metadataSize / 2));

  block_count_t newSize = config.physical_blocks + (metadataSize / 2);
  growVDOPhysical(newSize, vdo_map_to_system_error(VDO_INCREMENT_TOO_SMALL));
  validateSuperBlock();
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "add storage to a VDO with write error",    testAddStorageWithWriteError },
  { "add storage to a VDO",                     testAddStorage               },
  { "fail to add storage in recovery mode",     testAddStorageInRecoveryMode },
  { "fail to grow by a tiny amount",            testTooSmallGrowth           },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "VDO resize tests (Resize_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeResizeT1,
  .cleaner                  = tearDownRecoveryModeTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
