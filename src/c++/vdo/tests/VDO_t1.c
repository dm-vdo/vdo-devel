/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map.h"
#include "recovery-journal.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**
 * Test-specific initialization.
 *
 * @param dataFormatter  The data formatter to use for test data
 **/
static void initializeVDOT1(DataFormatter formatter)
{
  const TestParameters parameters = {
    .mappableBlocks      = 64,
    // The test assumes that logical blocks is 2 x mappable blocks.
    .logicalBlocks       = 128,
    .journalBlocks       = 16,
    .logicalThreadCount  = 3,
    .physicalThreadCount = 2,
    .hashZoneThreadCount = 2,
    .dataFormatter       = formatter,
  };
  initializeVDOTest(&parameters);
}

/**********************************************************************/
static void verifyRecoveryJournalState(sequence_number_t lastCleanTail)
{
  struct recovery_journal *journal = vdo->recovery_journal;

  // Ensure head, tail, and lastWriteAcknowledged show an empty journal.
  CU_ASSERT_EQUAL(lastCleanTail, journal->block_map_head);
  CU_ASSERT_EQUAL(lastCleanTail, journal->slab_journal_head);
  CU_ASSERT_EQUAL(lastCleanTail, journal->tail);
  CU_ASSERT_EQUAL(lastCleanTail, journal->last_write_acknowledged);

  // Ensure that all journal points are set correctly.
  CU_ASSERT_EQUAL(lastCleanTail, journal->append_point.sequence_number);
  CU_ASSERT_EQUAL(0,             journal->append_point.entry_count);
}

/**
 * Test filling a VDO completely and then writing some duplicate blocks.
 **/
static void testFill(void)
{
  initializeVDOT1(fillWithOffset);
  block_count_t  physicalBlocks = getTestConfig().config.physical_blocks;
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_overhead(vdo)
                  + getPhysicalBlocksFree(),
                  physicalBlocks);

  // Fill the physical space.
  block_count_t dataBlocks = computeDataBlocksToFill();
  CU_ASSERT_EQUAL(dataBlocks, populateBlockMapTree());
  block_count_t expectedOverhead = physicalBlocks - dataBlocks;
  dataBlocks++;
  writeAndVerifyData(0, 0, dataBlocks, 0, dataBlocks - 1);
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_overhead(vdo), expectedOverhead);

  // Verify that the physical space is full. These writes will still attempt to
  // dedupe, but ideally shouldn't record any advice (but hard to check that).
  writeData(dataBlocks + 1, dataBlocks + 1, 1, VDO_NO_SPACE);
  writeData(dataBlocks + 2, dataBlocks + 2, 1, VDO_NO_SPACE);
  writeData(dataBlocks + 1, dataBlocks + 1, 1, VDO_NO_SPACE);
  writeData(dataBlocks + 2, dataBlocks + 2, 1, VDO_NO_SPACE);

  // Fill the virtual space.
  struct vdo_config config = getTestConfig().config;
  writeAndVerifyData(dataBlocks, 0, config.logical_blocks - dataBlocks, 0,
                     dataBlocks - 1);

  // Overwrite some addresses with some duplicate data.
  // NOTE: we serialize these writes because transient write locking can cause
  // deduping writes on a full VDO to fail.
  for (block_count_t offset = 0; offset < dataBlocks; offset++) {
    writeData(16 + offset, offset, 1, VDO_SUCCESS);
  }
  verifyWrite(16, 0, dataBlocks, 0, dataBlocks - 1);

  /*
   * Physical block 1 should now be mapped from logical blocks 1 and 17.
   * Overwriting both of those logical blocks with 0 must free a block.
   */
  writeAndVerifyData(1, 0, 1, 0, dataBlocks - 1);
  writeAndVerifyData(17, 0, 1, 1, dataBlocks - 2);

  /*
   * Physical block 2 should now be mapped from logical blocks 2 and 18.
   * Overwriting both of those with another shared value (in this case 0x03)
   * must free a block.
   */
  writeAndVerifyData(2, 3, 1, 1, dataBlocks - 2);
  writeAndVerifyData(18, 3, 1, 2, dataBlocks - 3);

  // We should now be able to write 0x01 and 0x02 back to logical
  // blocks 17 and 18.
  // NOTE: we serialize these writes because transient write locking can cause
  // deduping writes on a full VDO to fail.
  writeAndVerifyData(17, 1, 1, 1, dataBlocks - 2);
  writeAndVerifyData(18, 2, 1, 0, dataBlocks - 1);

  struct recovery_journal *journal = vdo->recovery_journal;
  CU_ASSERT_EQUAL(journal->last_write_acknowledged,
                  journal->active_block->sequence_number);
  sequence_number_t savedTail = journal->tail;

  // Shut down the VDO & load it again.
  restartVDO(false);

  // Verify we filled the logical space.
  journal = vdo->recovery_journal;
  CU_ASSERT_EQUAL(vdo_get_recovery_journal_logical_blocks_used(journal),
                  config.logical_blocks);

  verifyRecoveryJournalState(savedTail);

  // Check that the data is as we left it.
  logical_block_number_t lbn = 0;
  verifyData(lbn++, 0, 1);
  verifyData(lbn++, 0, 1);
  verifyData(lbn++, 3, 1);
  verifyData(lbn++, 3, 1);
  verifyData(lbn, 4, 12);
  lbn += 12;
  verifyData(lbn, 0, dataBlocks);
  lbn += dataBlocks;
  verifyData(lbn, 16, config.logical_blocks - lbn);
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_overhead(vdo), expectedOverhead);

  restartVDO(true);

  // Verify that we don't see the old data.
  for (logical_block_number_t lbn = 0; lbn < config.logical_blocks; lbn++) {
    verifyData(lbn, 0, 1);
  }

  journal = vdo->recovery_journal;
  CU_ASSERT_EQUAL(vdo_get_recovery_journal_logical_blocks_used(journal), 0);
}

/**
 * Test deduplication of concurrent writes.
 **/
static void testInFlightDedupe(void)
{
  initializeVDOT1(fillAlternating);
  block_count_t blocksFree = populateBlockMapTree();

  writeAndVerifyData(0, 0, blocksFree, blocksFree - 2, 2);

  // Verify logical blocks used is correct.
  struct recovery_journal *journal = vdo->recovery_journal;
  CU_ASSERT_EQUAL(vdo_get_recovery_journal_logical_blocks_used(journal),
                  blocksFree);
}

/**
 * Fail a data write.
 *
 * Implements vdo_action_fn
 **/
static void failDataWrite(struct vdo_completion *completion)
{
  struct bio *bio = as_vio(completion)->bio;
  bio->bi_status = BLK_STS_VDO_INJECTED;
  bio->bi_end_io(bio);
}

/**
 * Replace the data_vio submission callback to always fail writes.
 *
 * Implements CompletionHook
 **/
static bool failDataWritesHook(struct vdo_completion *completion)
{
  if (isDataWrite(completion)) {
    completion->callback = failDataWrite;
  }

  return true;
}

/**
 * Test that a write error from the layer does not cause assertion failures
 * (VDO-1434).
 **/
static void testFailedWrite(void)
{
  initializeVDOT1(fillWithOffset);
  setCompletionEnqueueHook(failDataWritesHook);
  writeData(1, 1, 1, BLK_STS_VDO_INJECTED);
  setStartStopExpectation(VDO_READ_ONLY);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "fill an entire VDO",                      testFill           },
  { "test dedupe of simultaneous requests",    testInFlightDedupe },
  { "test that a failed write doesn't assert", testFailedWrite    },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "VDO read and write tests (VDO_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
