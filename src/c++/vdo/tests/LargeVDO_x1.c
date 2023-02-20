/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/**
 * This test simulates a VDO with a very large amount of physical
 * storage (currently 256 TB). In order to do that, a SparseLayer
 * defines the parts of the physical storage which must exist for the
 * VDO to function, which currently includes the super block, the
 * journal, the full reference count structure, and any data and block
 * map pages that are used. The undefined blocks will always read as
 * zeroes, so any changes to the VDO's on-disk structures may require
 * this test to be adjusted.
 *
 * This test currently requires ~65GB of memory and ~390GB of space on
 * /mnt/raid0 to run.
 **/

#include "albtest.h"

#include "memory-alloc.h"
#include "syscalls.h"
#include "testUtils.h"

#include "slab-depot.h"
#include "vdo.h"
#include "vdoConfig.h"

#include "asyncLayer.h"
#include "ioRequest.h"
#include "sparseLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

// Configure a very large VDO.
static const block_count_t  DATA_BLOCKS = 16;
static const char          *testFile    = "/mnt/raid0/large_vdo_temp";

// Configure a very large VDO.
// Blocks: 1 super, 4 journal, 84318377 block map, 16777216 refcount
static TestParameters parameters = {
  .physicalBlocks    = 1L << 36, // requires 16777216 refcount blocks
  .logicalBlocks     = 1L << 36, // requires 84318377 block map blocks
  .slabSize          = 1  << 23, // 8192 slabs
  .slabCount         = 8192,
  .slabJournalBlocks = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
  .dataFormatter     = fillWithOffset,
  .enableCompression = false,
};

/**
 * Start allocating from the highest numbered slab in each zone.
 *
 * @param depot   The depot
 **/
static void vdo_allocate_from_last_slab(struct slab_depot *depot)
{
	zone_count_t zone;

	for (zone = 0; zone < depot->zone_count; zone++) {
		vdo_allocate_from_allocator_last_slab(&depot->allocators[zone]);
	}
}

/**
 * Initialize the test data and set up the layer and VDO.
 **/
static void initializeLargeVDOX1(void)
{
  // Set up the sparse layer
  MappingRange ranges[2] = {
    // The super block, the journal, the block map pages, and a few data blocks
    {
      .start  = 0,
      .length = 85000000,
      .offset = 0,
    },
    // Some high-numbered data blocks and space for the reference counts
    {
      .start  = (1L << 36) - (1 << 24),
      .length = (1 << 24),
      .offset = 85000000,
    },
  };

  PhysicalLayer *synchronousLayer;
  VDO_ASSERT_SUCCESS(makeSparseLayer(testFile, parameters.physicalBlocks,
                                     2, ranges, &synchronousLayer));
  initializeTestWithSynchronousLayer(&parameters, synchronousLayer);

  // Format and start the VDO
  restartVDO(true);

  // Start allocating physical blocks from the high end of the range
  vdo_allocate_from_last_slab(vdo->depot);
}

/**
 * Write the given data, verify it can be read, and check block usage.
 *
 * @param startBlock           The logical block at which to start writing
 * @param index                The index of the data to write
 * @param blockCount           The number of blocks to write and verify
 * @param expectedBlocksUsed   The number of blocks used by VDO
 **/
static void writeAndVerify(logical_block_number_t startBlock,
                           block_count_t          index,
                           block_count_t          blockCount,
                           block_count_t          expectedBlocksUsed)
{
  writeData(startBlock, index, blockCount, VDO_SUCCESS);
  verifyData(startBlock, index, blockCount);
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.data_blocks_used, expectedBlocksUsed);
}

/**
 * Trim the given data, verify that it is zeroed, and check block usage.
 *
 * @param startBlock           The logical block at which to start writing
 * @param index                The index of the data to write
 * @param blockCount           The number of blocks to write and verify
 * @param expectedBlocksUsed   The number of blocks used by VDO
 **/
static void clearAndVerify(logical_block_number_t startBlock,
                           block_count_t          blockCount,
                           block_count_t          expectedBlocksUsed)
{
  discardData(startBlock, blockCount, VDO_SUCCESS);
  verifyZeros(startBlock, blockCount);
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.data_blocks_used, expectedBlocksUsed);
}

/**********************************************************************/
static void testBasic(void)
{
  // Write some data and demonstrate deduplication
  writeAndVerify(0, 1, DATA_BLOCKS, DATA_BLOCKS);
  writeAndVerify(DATA_BLOCKS, 1, DATA_BLOCKS, DATA_BLOCKS);
  writeAndVerify(2 * DATA_BLOCKS, 1, DATA_BLOCKS, DATA_BLOCKS);

  // Restart to test save/load
  restartVDO(false);

  // Crash and restart the VDO
  block_count_t  blockCount = layer->getBlockCount(layer);
  size_t         layerSize  = VDO_BLOCK_SIZE * blockCount;
  char          *buffer;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(layerSize, char, __func__, &buffer));
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, blockCount, buffer));
  stopVDO();
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, blockCount, buffer));
  UDS_FREE(buffer);
  startVDO(VDO_CLEAN);

  // Overwrite with zeros and reclaim space
  clearAndVerify(0, DATA_BLOCKS, DATA_BLOCKS);
  clearAndVerify(DATA_BLOCKS, DATA_BLOCKS, DATA_BLOCKS);
  clearAndVerify(2 * DATA_BLOCKS, DATA_BLOCKS, 0);
}

/**********************************************************************/

static CU_TestInfo largeVDOTests[] = {
  { "read/write large VDO",  testBasic  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo largeVDOSuite = {
  .name        = "Large VDO tests (LargeVDO_x1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeLargeVDOX1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = largeVDOTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &largeVDOSuite;
}
