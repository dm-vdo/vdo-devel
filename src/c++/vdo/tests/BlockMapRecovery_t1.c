/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "block-map.h"
#include "recovery.h"
#include "recovery-journal.h"
#include "slab-depot.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "completionUtils.h"
#include "journalWritingUtils.h"
#include "numberedBlockMapping.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // Use more logical blocks than fit on one block map page.
  BLOCK_COUNT = 8192,
};

static struct numbered_block_mapping *entries    = NULL;
static size_t                         entryCount = 0;

/**
 * Initialize the index, vdo, and test data.
 **/
static void initializeRecoveryTest(void)
{
  TestParameters parameters = {
    .logicalBlocks      = BLOCK_COUNT,
    .slabCount          = 1,
    .slabSize           = 1024,
    .logicalThreadCount = 1,
  };

  initializeVDOTest(&parameters);

  // Populate the entire block map tree, add slabs, then save and restart
  // the vdo.
  populateBlockMapTree();
  block_count_t dataBlocks = vdo->depot->slab_config.data_blocks;
  addSlabs(DIV_ROUND_UP(BLOCK_COUNT * 2, dataBlocks));
  restartVDO(false);

  initializeJournalWritingUtils(vdo->recovery_journal->size, BLOCK_COUNT, 1);
}

/**
 * Destroy the test data, vdo, and index session.
 **/
static void teardownRecoveryTest(void)
{
  UDS_FREE(entries);
  tearDownJournalWritingUtils();
  tearDownVDOTest();
}

/**
 * Allocate and generate a numbered block mapping array with the given number
 * of mappings, updating the expected block map mappings as the array is
 * generated. The pattern used to fill the array is different from the pattern
 * used to fill the block map with known mappings.
 *
 * @param mappingCount  The number of mappings to put in the array
 **/
static void generateNumberedBlockMappings(block_count_t mappingCount)
{
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(mappingCount, struct numbered_block_mapping,
                                  __func__, &entries));

  struct block_map *map           = vdo->block_map;
  block_count_t     logicalBlocks = getTestConfig().config.logical_blocks;
  for (block_count_t entry = 0; entry < mappingCount; entry++) {
    struct numbered_block_mapping *mapping = &entries[entry];
    logical_block_number_t         lbn     = (entry * 3) % logicalBlocks;
    page_count_t pageIndex = lbn / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
    mapping->block_map_slot = (struct block_map_slot) {
      .pbn  = vdo_find_block_map_page_pbn(map, pageIndex),
      .slot = lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
    };

    physical_block_number_t pbn = computePBNFromLBN(lbn, 1);
    mapping->block_map_entry
      = vdo_pack_block_map_entry(pbn, VDO_MAPPING_STATE_UNCOMPRESSED);
    mapping->number = entry;
    setBlockMapping(lbn, pbn, VDO_MAPPING_STATE_UNCOMPRESSED);
  }
}

/**********************************************************************/
static void recoverAction(struct vdo_completion *completion)
{
  recover_block_map(vdo, entryCount, entries, completion);
}

/**
 * Test block map recovery by verifying block map state after a recovery with
 * a known mapping array pattern.
 *
 * @param desiredEntryCount  The number of mappings to generate and replay
 */
static void testRecovery(size_t desiredEntryCount)
{
  // Fill the block map with known mappings and make sure they can be read out.
  putBlocksInMap(0, BLOCK_COUNT);
  verifyBlockMapping(0);

  /*
   * Generate a mapping array to feed into block map recovery, simulating
   * recovery or rebuild extracting increfs from the journal, and update
   * the expected block map mapping array with these mappings.
   */
  entryCount = desiredEntryCount;
  generateNumberedBlockMappings(entryCount);

  // Do a block map recovery.
  performSuccessfulActionOnThread(recoverAction, vdo->thread_config.logical_threads[0]);

  // Verify that all block map mappings are either the original value or the
  // new mapping expected from recovery.
  verifyBlockMapping(0);
}

/**
 * Test a block map recovery with no new mappings.
 **/
static void testEmpty(void)
{
  testRecovery(0);
}

/**
 * Test a block map recovery touching every third LBN once.
 **/
static void testThird(void)
{
  testRecovery(getTestConfig().config.logical_blocks / 3);
}

/**
 * Test a block map recovery touching every LBN once.
 **/
static void testAll(void)
{
  testRecovery(getTestConfig().config.logical_blocks);
}

/**
 * Test a block map recovery touching every LBN three times.
 **/
static void testMultiple(void)
{
  testRecovery(getTestConfig().config.logical_blocks * 3);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "empty list of mappings",            testEmpty    },
  { "touching one-third of LBNs",        testThird    },
  { "touching every LBN",                testAll      },
  { "touching every LBN multiple times", testMultiple },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Recover the block map (BlockMapRecovery_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeRecoveryTest,
  .cleaner                  = teardownRecoveryTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
