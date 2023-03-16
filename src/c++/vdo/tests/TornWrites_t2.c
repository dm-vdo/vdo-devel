/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "encodings.h"
#include "slab-depot.h"
#include "types.h"

#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static const TestParameters TEST_PARAMETERS = {
  .journalBlocks     = 32,
  .slabCount         = 1,
  .slabSize          = 8192,
  .slabJournalBlocks = 32,
  .dataFormatter     = fillWithOffsetPlusOne
};

/**
 * Set up the test.
 **/
static void initializeTornWritesT2(void)
{
  initializeVDOTest(&TEST_PARAMETERS);
}

/**
 * Assert that the commit points for two sector indexes are either equal
 * or not equal.
 *
 * @param block             The reference block
 * @param sector1           The index of the first sector
 * @param sector2           The index of the second sector
 * @param expectedEquality  Whether the commit points must be equal
 **/
static void assertCommitPointComparison(struct reference_block *block,
                                        uint8_t                 sector1,
                                        uint8_t                 sector2,
                                        bool                    expectedEquality)
{
  CU_ASSERT_EQUAL(areJournalPointsEqual(block->commit_points[sector1],
                                        block->commit_points[sector2]),
                  expectedEquality);
}

/**
 * Test the effect of a torn write on the slab's reference counts.
 **/
static void testReferenceCountTornWrite(void)
{
  populateBlockMapTree();
  block_count_t initialBlocks = fillPhysicalSpace(0, 0);
  addSlabs(1);
  block_count_t newBlocks = getPhysicalBlocksFree();

  // Fill the first two sectors of the first reference block in slab 1.
  writeData(initialBlocks, initialBlocks, COUNTS_PER_SECTOR * 2, VDO_SUCCESS);
  logical_block_number_t lbn = initialBlocks + (COUNTS_PER_SECTOR * 2);

  // Record the pbn of the reference block to be torn
  physical_block_number_t pbn = vdo->depot->slabs[1]->ref_counts_origin;

  // Save out the VDO so that the torn write will matter
  stopVDO();

  // Read the contents of the reference block
  char           buffer[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, pbn, 1, buffer));

  // Restart the VDO
  startVDO(VDO_CLEAN);

  // Write duplicates of the blocks in the first half of each of the two
  // sectors
  writeData(lbn, initialBlocks, COUNTS_PER_SECTOR / 2, VDO_SUCCESS);
  lbn += COUNTS_PER_SECTOR / 2;
  writeData(lbn, initialBlocks + COUNTS_PER_SECTOR, COUNTS_PER_SECTOR / 2,
            VDO_SUCCESS);
  lbn += COUNTS_PER_SECTOR / 2;

  // Archive the state of the first reference block in slab 1.
  struct ref_counts      *refCounts   = vdo->depot->slabs[1]->reference_counts;
  struct reference_block *referenceBlock
    = vdo_get_reference_block(refCounts, 0);
  CU_ASSERT_EQUAL(COUNTS_PER_SECTOR * 2, referenceBlock->allocated_count);
  vdo_refcount_t counts[COUNTS_PER_BLOCK];
  memcpy(counts, vdo_get_reference_counters_for_block(referenceBlock),
         COUNTS_PER_BLOCK * sizeof(vdo_refcount_t));

  // Make the torn reference block for the block we are going to tear,
  // failing to write the second and last sectors.
  struct packed_reference_block packedReferenceBlock;
  vdo_pack_reference_block(referenceBlock, &packedReferenceBlock);
  memcpy(&packedReferenceBlock.sectors[1], buffer + VDO_SECTOR_SIZE,
         VDO_SECTOR_SIZE);
  memcpy(&packedReferenceBlock.sectors[7], buffer + (7 * VDO_SECTOR_SIZE),
         VDO_SECTOR_SIZE);

  // Crash the VDO and then simulate a tear in the write of the first
  // reference block.
  crashVDO();

  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1,
                                   (char *) &packedReferenceBlock));

  // Restart the VDO and confirm that the tear was repaired.
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  refCounts      = vdo->depot->slabs[1]->reference_counts;
  referenceBlock = vdo_get_reference_block(refCounts, 0);

  // Ensure we have a torn write, with sectors 1 and 7 having an old commit
  // point.
  assertCommitPointComparison(referenceBlock, 0, 1, false);
  assertCommitPointComparison(referenceBlock, 0, 2, true);
  assertCommitPointComparison(referenceBlock, 0, 3, true);
  assertCommitPointComparison(referenceBlock, 0, 4, true);
  assertCommitPointComparison(referenceBlock, 0, 5, true);
  assertCommitPointComparison(referenceBlock, 0, 6, true);
  assertCommitPointComparison(referenceBlock, 0, 7, false);
  assertCommitPointComparison(referenceBlock, 1, 7, true);
  CU_ASSERT_EQUAL(COUNTS_PER_SECTOR * 2, referenceBlock->allocated_count);
  UDS_ASSERT_EQUAL_BYTES(counts,
                         vdo_get_reference_counters_for_block(referenceBlock),
                         COUNTS_PER_BLOCK * sizeof(vdo_refcount_t));

  // Trim all of the previous writes to confirm that we don't underflow
  // decrefs.
  discardData(0, lbn, VDO_SUCCESS);
  verifyZeros(0, lbn);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), initialBlocks + newBlocks);
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_allocated(vdo), 0);
}

/**********************************************************************/
static CU_TestInfo tornWriteTests[] = {
  { "test reference block torn write", testReferenceCountTornWrite },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo tornWriteSuite = {
  .name                     = "Torn reference block writes (TornWrites_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeTornWritesT2,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tornWriteTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &tornWriteSuite;
}
