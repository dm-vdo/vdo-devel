/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdlib.h>

#include "albtest.h"

#include "memory-alloc.h"

#include "block-allocator.h"
#include "data-vio.h"
#include "read-only-notifier.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab-scrubber.h"
#include "status-codes.h"
#include "vdo.h"
#include "vdo-component-states.h"
#include "vdo-layout.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "completionUtils.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  VDO_LAYOUT_START    = 2, // Covers geometry, index, and super blocks.
  SLAB_SIZE           = 16,
  // This is not a slab size multiple so the runt slab test will be meaningful,
  // and is bigger than 6 so the slab ring rebuild test has 6 slabs.
  BLOCK_COUNT         = ((7 * SLAB_SIZE) - 1),
  SLAB_JOURNAL_BLOCKS = 8,
  JOURNAL_BLOCKS      = 16,
  LARGE_SLAB_SIZE     = (1 << 10),
  LARGE_BLOCK_COUNT   = (1 << 14),
};

static struct slab_depot         *depot;
static struct block_allocator    *allocator;
static struct slab_depot         *decodedDepot;
static struct slab_config         slabConfig;
static struct journal_point       recoveryJournalPoint;
static physical_block_number_t    firstBlock;
static size_t			  size;
static block_count_t              overhead;


/**
 * Make the default allocator.
 **/
static void initializeAllocatorT1(block_count_t slabSize,
                                  block_count_t blockCount)
{
  overhead = (VDO_LAYOUT_START + JOURNAL_BLOCKS
              + VDO_SLAB_SUMMARY_BLOCKS
              + DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);

  TestParameters parameters = {
    .slabSize          = slabSize,
    .physicalBlocks    = overhead + blockCount,
    .journalBlocks     = JOURNAL_BLOCKS,
    .slabJournalBlocks = SLAB_JOURNAL_BLOCKS,
  };

  initializeVDOTest(&parameters);

  depot      = vdo->depot;
  allocator  = depot->allocators[0];
  slabConfig = depot->slab_config;
  firstBlock = depot->first_block;

  recoveryJournalPoint = (struct journal_point) {
    .sequence_number = 1,
    .entry_count     = 0,
  };
}

/**
 * Translate an abstract data block number to the physical_block_number_t
 * of the block within the block allocator partition.
 *
 * @param dataBlockNumber  The index of the data block in a virtual array
 *                         consisting only of data blocks ordered by PBN
 *
 * @return the expected physical block number of the specified data block
 **/
static physical_block_number_t dataBlockNumberToPBN(size_t dataBlockNumber)
{
  size_t slabIndex  = dataBlockNumber / slabConfig.data_blocks;
  size_t slabOffset = dataBlockNumber % slabConfig.data_blocks;
  return (firstBlock + (slabIndex * slabConfig.slab_blocks) + slabOffset);
}

/**
 * Translate the physical block number of a data block to an abstract data
 * block number.
 *
 * @param pbn  The physical block number of a data block
 *
 * @return  The index of the data block in a virtual array
 *          consisting only of data blocks ordered by PBN
 **/
static size_t pbnToDataBlockNumber(physical_block_number_t pbn)
{
  size_t slabIndex  = (pbn - firstBlock) / slabConfig.slab_blocks;
  size_t slabOffset = (pbn - firstBlock) % slabConfig.slab_blocks;
  return ((slabIndex * slabConfig.data_blocks) + slabOffset);
}

/**
 * Assert that the block allocator fails to allocate space with a VDO_NO_SPACE
 * status code.
 **/
static void assertNoSpace(void)
{
  CU_ASSERT_EQUAL(0, getPhysicalBlocksFree());
  physical_block_number_t allocatedBlock;
  CU_ASSERT_EQUAL(VDO_NO_SPACE,
                  vdo_allocate_block(allocator, &allocatedBlock));
}

/**
 * The action to add an entry to the journal.
 *
 * @param completion A VIO for which to add an entry
 **/
static void addSlabJournalEntryAction(struct vdo_completion *completion)
{
  struct data_vio *dataVIO;
  struct reference_updater *updater;
  if (completion->type == VIO_COMPLETION) {
    dataVIO = as_data_vio(completion);
    updater = &dataVIO->increment_updater;
  } else {
    dataVIO = container_of(completion, struct data_vio, decrement_completion);
    updater = &dataVIO->decrement_updater;
  }

  // These journal point don't correspond to anything real since there
  // is no recovery journal in this test; they simply need to unique.
  recoveryJournalPoint.entry_count++;
  dataVIO->recovery_journal_point = recoveryJournalPoint;
  vdo_add_slab_journal_entry(vdo_get_slab(depot, updater->zpbn.pbn)->journal,
                             completion,
                             updater);
}

/**
 * Allocate a block, increment its reference count, and make an appropriate
 * slab journal entry to use it.
 *
 * @return the block which was allocated
 **/
static physical_block_number_t useNextBlock(void)
{
  physical_block_number_t allocatedBlock;
  VDO_ASSERT_SUCCESS(vdo_allocate_block(allocator, &allocatedBlock));

  struct data_vio dataVIO = {
    .vio = {
      .type = VIO_TYPE_DATA,
    },
    .new_mapped = {
      .pbn = allocatedBlock,
    },
    .increment_updater = {
      .operation = VDO_JOURNAL_DATA_REMAPPING,
      .increment = true,
      .zpbn = {
        .pbn   = allocatedBlock,
        .state = VDO_MAPPING_STATE_UNCOMPRESSED,
      },
    },
  };

  struct vdo_completion *completion = &dataVIO.vio.completion;
  vdo_initialize_completion(completion, vdo, VIO_COMPLETION);
  VDO_ASSERT_SUCCESS(performWrappedAction(addSlabJournalEntryAction, completion));
  return allocatedBlock;
}

/**
 * Decrement the reference count of a block and make an appropriate slab
 * journal entry for the decrement.
 *
 * @param pbn  The physical block number of the block
 **/
static void decRef(physical_block_number_t pbn)
{
  struct data_vio dataVIO = {
    .vio = {
      .type = VIO_TYPE_DATA,
    },
    .mapped = {
      .pbn = pbn,
    },
    .decrement_updater = {
      .operation = VDO_JOURNAL_DATA_REMAPPING,
      .increment = false,
      .zpbn = {
        .pbn   = pbn,
        .state = VDO_MAPPING_STATE_UNCOMPRESSED,
      },
    },
  };

  struct vdo_completion *completion = &dataVIO.decrement_completion;
  vdo_initialize_completion(completion, vdo, VDO_DECREMENT_COMPLETION);
  VDO_ASSERT_SUCCESS(performWrappedAction(addSlabJournalEntryAction, completion));
}

/**
 * Allocate consecutive blocks in a given range. This function will
 * succeed only when exactly 'start' blocks have already been allocated,
 * and at least ('end' - 'start') blocks are free.
 *
 * @param start The number of the first block to allocate (inclusive)
 * @param end   The number of the last block to allocate (exclusive)
 **/
static void allocateSimply(unsigned int start, unsigned int end)
{
  CU_ASSERT_EQUAL(end - start, getPhysicalBlocksFree());
  for (unsigned int i = start; i < end; i++) {
    CU_ASSERT_EQUAL(dataBlockNumberToPBN(i), useNextBlock());
  }
  assertNoSpace();
}

/**********************************************************************/
static block_count_t getDataBlockCount(block_count_t blockCount)
{
  block_count_t totalSlabBlocks = blockCount;

  // Count the number of complete slabs. There is no runt slab.
  size_t slabCount = totalSlabBlocks / slabConfig.slab_blocks;
  CU_ASSERT_EQUAL(slabCount, allocator->slab_count);
  totalSlabBlocks = slabCount * slabConfig.slab_blocks;

  // XXX should we test slab configuration measurements here or
  // elsewhere instead of just trusting them?
  return (slabCount * slabConfig.data_blocks);
}

/**
 * Action to prepare the slab depot to come online.
 *
 * @param completion  The action completion
 **/
static void prepareDepotAction(struct vdo_completion *completion)
{
  vdo_prepare_slab_depot_to_allocate(decodedDepot, VDO_SLAB_DEPOT_NORMAL_LOAD,
                                     completion);
}

/**
 * Check that two depot states are the same.
 *
 * @param a  the first state to compare
 * @param b  the second state to compare
 **/
static void assertSameStates(struct slab_depot_state_2_0 a,
                             struct slab_depot_state_2_0 b)
{
  CU_ASSERT_EQUAL(a.slab_config.slab_blocks, b.slab_config.slab_blocks);
  CU_ASSERT_EQUAL(a.slab_config.data_blocks, b.slab_config.data_blocks);
  CU_ASSERT_EQUAL(a.slab_config.reference_count_blocks,
                  b.slab_config.reference_count_blocks);
  CU_ASSERT_EQUAL(a.slab_config.slab_journal_blocks,
                  b.slab_config.slab_journal_blocks);
  CU_ASSERT_EQUAL(a.slab_config.slab_journal_flushing_threshold,
                  b.slab_config.slab_journal_flushing_threshold);
  CU_ASSERT_EQUAL(a.slab_config.slab_journal_blocking_threshold,
                  b.slab_config.slab_journal_blocking_threshold);
  CU_ASSERT_EQUAL(a.slab_config.slab_journal_scrubbing_threshold,
                  b.slab_config.slab_journal_scrubbing_threshold);
  CU_ASSERT_EQUAL(a.first_block, b.first_block);
  CU_ASSERT_EQUAL(a.last_block, b.last_block);
  CU_ASSERT_EQUAL(a.zone_count, b.zone_count);
}

/**********************************************************************/
static bool are_equivalent_slab_depots(struct slab_depot *depot_a,
                                       struct slab_depot *depot_b)
{
	size_t i;

	if ((depot_a->first_block != depot_b->first_block) ||
	    (depot_a->last_block != depot_b->last_block) ||
	    (depot_a->slab_count != depot_b->slab_count) ||
	    (depot_a->slab_size_shift != depot_b->slab_size_shift) ||
	    (vdo_get_slab_depot_allocated_blocks(depot_a) !=
	     vdo_get_slab_depot_allocated_blocks(depot_b))) {
		return false;
	}

	for (i = 0; i < depot_a->slab_count; i++) {
		struct vdo_slab *slab_a = depot_a->slabs[i];
		struct vdo_slab *slab_b = depot_b->slabs[i];

		if ((slab_a->start != slab_b->start) ||
		    (slab_a->end != slab_b->end) ||
		    !vdo_are_equivalent_ref_counts(slab_a->reference_counts,
						   slab_b->reference_counts)) {
			return false;
		}
	}

	return true;
}

/**
 * Check that encoding and decoding a slab depot works correctly.
 **/
static void verifyCoding(void)
{
  performSuccessfulDepotActionOnDepot(depot, VDO_ADMIN_STATE_SAVING);

  struct slab_depot_state_2_0 state = vdo_record_slab_depot(depot);
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(SLAB_DEPOT_COMPONENT_ENCODED_SIZE, &buffer));
  VDO_ASSERT_SUCCESS(encode_slab_depot_state_2_0(state, buffer));

  struct slab_depot_state_2_0 decoded;
  VDO_ASSERT_SUCCESS(decode_slab_depot_state_2_0(buffer, &decoded));
  free_buffer(UDS_FORGET(buffer));

  assertSameStates(state, decoded);
  struct partition *slabSummaryPartition =
    vdo_get_partition(vdo->layout, VDO_SLAB_SUMMARY_PARTITION);
  VDO_ASSERT_SUCCESS(vdo_decode_slab_depot(decoded,
                                           vdo,
                                           slabSummaryPartition,
                                           &decodedDepot));
  performSuccessfulDepotActionOnDepot(decodedDepot, VDO_ADMIN_STATE_LOADING);
  performSuccessfulAction(prepareDepotAction);
  CU_ASSERT_TRUE(are_equivalent_slab_depots(depot, decodedDepot));

  vdo_free_slab_depot(UDS_FORGET(decodedDepot));
  performSuccessfulDepotActionOnDepot(depot, VDO_ADMIN_STATE_RESUMING);
}

/**
 * Test allocation with no reclaimable blocks
 **/
static void testSimpleAllocation(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);
  allocateSimply(0, getDataBlockCount(BLOCK_COUNT));
  verifyCoding();
}

/**
 * Free every third block, and then allocate them again.
 **/
static void reallocateEveryThird(block_count_t blockCount)
{
  block_count_t dataBlockCount = getDataBlockCount(blockCount);
  for (size_t dbn = 0; dbn < dataBlockCount; dbn += 3) {
    decRef(dataBlockNumberToPBN(dbn));
  }

  verifyCoding();

  // The blocks might not be re-allocated in the exact order we freed them.
  for (size_t i = 0; i < dataBlockCount; i += 3) {
    physical_block_number_t allocatedBlock = useNextBlock();
    physical_block_number_t dataBlockNumber
      = pbnToDataBlockNumber(allocatedBlock);
    CU_ASSERT_EQUAL(0, (dataBlockNumber % 3));
    struct vdo_slab *slab = vdo_get_slab(depot, allocatedBlock);
    enum reference_status status;
    VDO_ASSERT_SUCCESS(vdo_get_reference_status(slab->reference_counts,
                                                allocatedBlock, &status));
    CU_ASSERT_EQUAL(RS_SINGLE, status);
    verifyCoding();
  }

  assertNoSpace();
}

/**
 * Action to prepare to resize a slab depot.
 *
 * @param completion  The action completion
 **/
static void prepareResizeAction(struct vdo_completion *completion)
{
  VDO_ASSERT_SUCCESS(vdo_prepare_to_grow_slab_depot(depot, size));
  vdo_complete_completion(completion);
}

/**
 * Allocate all blocks, release some, and reallocate them.
 **/
static void testReallocation(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);
  allocateSimply(0, getDataBlockCount(BLOCK_COUNT));
  reallocateEveryThird(BLOCK_COUNT);

  block_count_t allocatedBlocks = vdo_get_slab_depot_allocated_blocks(depot);
  growVDOPhysical(getTestConfig().config.physical_blocks + BLOCK_COUNT,
                  VDO_SUCCESS);

  // resize reorders the slabs, so we can't predict the order
  block_count_t dataBlocks = getDataBlockCount(2 * BLOCK_COUNT);
  for (block_count_t i = allocatedBlocks; i < dataBlocks; i++) {
    useNextBlock();
  }
  assertNoSpace();

  reallocateEveryThird(2 * BLOCK_COUNT);
  verifyCoding();
}

/**
 * Resize an allocator by a delta large enough to change the number of
 * allocator/refCounts metadata blocks needed.
 **/
static void testLargeResize(void)
{
  initializeAllocatorT1(LARGE_SLAB_SIZE, LARGE_BLOCK_COUNT);

  // Allocate every block in the large allocator.
  block_count_t dataBlocks = getDataBlockCount(LARGE_BLOCK_COUNT);
  allocateSimply(0, dataBlocks);
  block_count_t allocatedBlocks = vdo_get_slab_depot_allocated_blocks(depot);
  CU_ASSERT_EQUAL(dataBlocks, allocatedBlocks);

  // Double the size of the allocator with resize.
  growVDOPhysical(getTestConfig().config.physical_blocks + LARGE_BLOCK_COUNT,
		  VDO_SUCCESS);

  dataBlocks = getDataBlockCount(2 * LARGE_BLOCK_COUNT);
  block_count_t dataBlocksAdded = dataBlocks - allocatedBlocks;

  // Make sure we actually grew the allocator significantly.
  CU_ASSERT_TRUE(dataBlocksAdded >= LARGE_BLOCK_COUNT / 2);

  // Some of the blocks that we added must have been used for metadata.
  CU_ASSERT_BETWEEN(dataBlocksAdded, LARGE_BLOCK_COUNT - LARGE_SLAB_SIZE,
                    LARGE_BLOCK_COUNT);

  // Every data block that we added must be free.
  CU_ASSERT_EQUAL(dataBlocksAdded, getPhysicalBlocksFree());

  // Resize scrambles the slab order, so we can't predict it.
  for (block_count_t i = allocatedBlocks; i < dataBlocks; i++) {
    useNextBlock();
  }
  assertNoSpace();

  // Our slabs must be numbered in order.
  slab_count_t slabCount = depot->slab_count;
  for (slab_count_t slabNumber = 0; slabNumber < slabCount; slabNumber++) {
    CU_ASSERT_EQUAL(slabNumber, depot->slabs[slabNumber]->slab_number);
  }
  verifyCoding();
}

/**
 * Resize an allocator twice, then undo it.
 **/
static void testUndoResize(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);
  const block_count_t BLOCK_DELTA = SLAB_SIZE * 20 + 3;
  // Grow the slab depot manually, but don't use the new slabs yet.
  size = BLOCK_COUNT + BLOCK_DELTA;
  performSuccessfulAction(prepareResizeAction);

  // Give up on growing the new slabs.
  vdo_abandon_new_slabs(depot);

  allocateSimply(0, getDataBlockCount(BLOCK_COUNT));
  verifyCoding();
}

/**
 * Ensure there are no runt slabs allocated.
 **/
static void testNoRuntSlabs(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);
  CU_ASSERT_EQUAL(depot->slab_count, BLOCK_COUNT / SLAB_SIZE);
  CU_ASSERT_EQUAL(depot->last_block - depot->first_block,
                  depot->slab_count * SLAB_SIZE);
}

/**
 * Set a slab's cleanliness and emptiness in the slab summary.
 *
 * @param slabNumber    The index of the slab in question
 * @param cleanliness   Whether the slab is clean
 * @param emptiness     The number of free blocks in the slab
 **/
static void setSlabSummaryEntry(slab_count_t slabNumber,
                                bool         cleanliness,
                                size_t       emptiness)
{
  performSlabSummaryUpdate(allocator->summary, slabNumber,
                           slabNumber % SLAB_JOURNAL_BLOCKS, true,
                           cleanliness, emptiness);
}

/**
 * Chop a list and return the chopped entry as a slab.
 *
 * @param list  The list to chop
 *
 * @return The slab that was chopped off the list or NULL if the list was empty
 **/
static struct vdo_slab *chopSlab(struct list_head *list)
{
  struct vdo_slab *slab
    = list_first_entry_or_null(list, struct vdo_slab, allocq_entry);
  if (slab != NULL) {
    list_del_init(&slab->allocq_entry);
  }
  return slab;
}

/**
 * Test that the unrecovered slab ring, populated during recovery out of the
 * slab summary, is correctly created.
 **/
static void testUnrecoveredSlabs(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);
  /*
   * We will set the slab summary to believe the following about the 6 slabs:
   * vdo_slab 4: clean, 16 free blocks
   * vdo_slab 0: clean,  8 free blocks
   * vdo_slab 2: clean,  0 free blocks
   * vdo_slab 5: dirty,  8 free blocks
   * vdo_slab 1: dirty,  4 free blocks
   * vdo_slab 3: dirty,  0 free blocks
   * After building the slab rings from the slab summary, popping slabs off
   * the unrecovered slab ring should give slabs in the order 5, 1, 3, NULL.
   */
  CU_ASSERT_TRUE(allocator->slab_count > 5);
  reset_priority_table(allocator->prioritized_slabs);
  for (slab_count_t i = 0; i < depot->slab_count; i++) {
    struct vdo_slab *slab = depot->slabs[i];
    INIT_LIST_HEAD(&slab->allocq_entry);
  }

  setSlabSummaryEntry(4, true,  SLAB_SIZE);
  setSlabSummaryEntry(0, true,  SLAB_SIZE / 2);
  setSlabSummaryEntry(2, true,  0);
  setSlabSummaryEntry(5, false, SLAB_SIZE / 2);
  setSlabSummaryEntry(1, false, SLAB_SIZE / 4);
  setSlabSummaryEntry(3, false, 0);

  depot->load_type = VDO_SLAB_DEPOT_RECOVERY_LOAD;
  VDO_ASSERT_SUCCESS(vdo_prepare_slabs_for_allocation(allocator));

  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 4);
  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 0);
  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 2);
  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 5);
  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 1);
  CU_ASSERT_EQUAL(chopSlab(&allocator->slab_scrubber->slabs)->slab_number, 3);
  CU_ASSERT_PTR_NULL(chopSlab(&allocator->slab_scrubber->slabs));
}

/**
 * Check that the block allocator avoids opening a new slab if there is a free
 * block still available in a previously-open slab.
 **/
static void testAllocationPolicy(void)
{
  initializeAllocatorT1(SLAB_SIZE, BLOCK_COUNT);

  // Allocation should start at the first block in slab zero and continue
  // sequentially and contiguously until the slab is filled.
  physical_block_number_t slabZeroStart = firstBlock;
  for (slab_block_number sbn = 0; sbn < slabConfig.data_blocks; sbn++) {
    physical_block_number_t pbn = slabZeroStart + sbn;
    CU_ASSERT_EQUAL(pbn, useNextBlock());
  }

  // vdo_slab zero was filled, so slab one should be opened.
  physical_block_number_t slabOneStart = slabZeroStart + slabConfig.slab_blocks;

  // Keep cycling through slab one, allocating and freeing each block in turn
  // a few times, verifying that the open slab stays open until it is filled.
  for (unsigned int cycle = 0; cycle < 4; cycle++) {
    for (slab_block_number sbn = 0; sbn < slabConfig.data_blocks; sbn++) {
      physical_block_number_t pbn = slabOneStart + sbn;
      CU_ASSERT_EQUAL(pbn, useNextBlock());
      decRef(pbn);
    }
  }

  // Fill slab one.
  for (slab_block_number sbn = 0; sbn < slabConfig.data_blocks; sbn++) {
    physical_block_number_t pbn = slabOneStart + sbn;
    CU_ASSERT_EQUAL(pbn, useNextBlock());
  }

  // Go back to slab zero and free all the blocks in it.
  for (slab_block_number sbn = 0; sbn < slabConfig.data_blocks; sbn++) {
    decRef(slabZeroStart + sbn);
  }

  // vdo_slab zero is empty; slab one is full. Allocation should re-open slab
  // zero (which we will fill) instead of opening unopened slab two.
  for (slab_block_number sbn = 0; sbn < slabConfig.data_blocks; sbn++) {
    physical_block_number_t pbn = slabZeroStart + sbn;
    CU_ASSERT_EQUAL(pbn, useNextBlock());
  }

  // Free one block in slab one.
  decRef(slabOneStart);

  // With only one free block in slab one, the allocator should prefer to
  // break open slab two instead of searching slab one for a single block.
  physical_block_number_t slabTwoStart = slabOneStart + slabConfig.slab_blocks;
  CU_ASSERT_EQUAL(slabTwoStart, useNextBlock());

  // Keep allocating until only one block remains.
  while (getPhysicalBlocksFree() > 1) {
    useNextBlock();
  }

  // With all the unopened slabs exhausted, the only remaining free block,
  // the first block in slab one, must at last be found and allocated.
  CU_ASSERT_EQUAL(slabOneStart, useNextBlock());
}

/**********************************************************************/
static CU_TestInfo allocatorTests[] = {
  { "allocation with no freed blocks",       testSimpleAllocation },
  { "allocation after freeing some blocks",  testReallocation     },
  { "resize a larger allocator",             testLargeResize      },
  { "grow then shrink an allocator",         testUndoResize       },
  { "no runt slabs",                         testNoRuntSlabs      },
  { "unrecovered slab ring population",      testUnrecoveredSlabs },
  { "allocation policy",                     testAllocationPolicy },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo allocatorSuite = {
  .name                     = "Allocator tests (BlockAllocator_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = allocatorTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &allocatorSuite;
}
