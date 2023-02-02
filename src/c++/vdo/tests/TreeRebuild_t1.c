/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map.h"
#include "forest.h"
#include "slab-depot.h"
#include "vdo-component-states.h"

#include "vdoConfig.h"

#include "adminUtils.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct block_map_tree_zone *zone;

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  TestParameters parameters = {
    .mappableBlocks      = 256,
    .logicalBlocks       = (VDO_BLOCK_MAP_ENTRIES_PER_PAGE
                            * DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 2),
    .logicalThreadCount  = 1,
    .physicalThreadCount = 2,
    .hashZoneThreadCount = 1,
    .dataFormatter       = fillWithOffsetPlusOne,
  };

  initializeVDOTest(&parameters);
  zone = &vdo->block_map->zones[0].tree_zone;
}

/**
 * Return a particular block map page from the given root.
 **/
static struct block_map_page *getTreePageFromForest(struct forest *forest,
                                                    root_count_t   rootIndex)
{
  return vdo_as_block_map_page(vdo_get_tree_page_by_index(forest, rootIndex, 2,
                                                          0));
}

/**
 * Rewrite a particular entry in the given root with the given PBN.
 **/
static void corruptTreePageInForest(struct forest            *forest,
                                    root_count_t              rootIndex,
                                    slot_number_t             slot,
                                    enum block_mapping_state  state,
                                    physical_block_number_t   pbn)
{
  struct tree_page      *treePage = vdo_get_tree_page_by_index(forest,
                                                               rootIndex,
                                                               2, 0);
  struct block_map_page *page     = vdo_as_block_map_page(treePage);
  page->entries[slot]             = vdo_pack_block_map_entry(pbn, state);
  vdo_write_tree_page(treePage, zone);
}

/**
 * Introduce a variety of corruptions into the block map tree.
 **/
static void corruptMapAction(struct vdo_completion *completion)
{
  struct forest *forest = vdo->block_map->forest;

  // Page reference is completely out of range.
  corruptTreePageInForest(forest, 2, 1, VDO_MAPPING_STATE_UNCOMPRESSED,
                          getTestConfig().config.physical_blocks + 1);

  // Page reference points to slab metadata.
  corruptTreePageInForest(forest, 4, 10, VDO_MAPPING_STATE_UNCOMPRESSED,
                          vdo->depot->last_block - 2);

  // Page reference points at a root node.
  corruptTreePageInForest(forest, 6, 20, VDO_MAPPING_STATE_UNCOMPRESSED, 10);

  // Page reference points at a previously referenced tree page.
  struct block_map_page *page = getTreePageFromForest(forest, 0);
  physical_block_number_t treePBN
    = vdo_unpack_block_map_entry(&page->entries[0]).pbn;
  corruptTreePageInForest(forest, 8, 100, VDO_MAPPING_STATE_UNCOMPRESSED,
                          treePBN);

  // Page reference is unmapped but has a valid non-zero PBN.
  corruptTreePageInForest(forest, 10, (VDO_BLOCK_MAP_ENTRIES_PER_PAGE / 2),
                          VDO_MAPPING_STATE_UNMAPPED,
                          vdo->depot->first_block);

  // Page reference is compressed but has no PBN.
  corruptTreePageInForest(forest, 12, (VDO_BLOCK_MAP_ENTRIES_PER_PAGE - 1),
                          VDO_MAPPING_STATE_COMPRESSED_MAX, 0);

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Assert that an entry is properly unmapped.
 **/
static void validateUnmappedEntry(root_count_t root, slot_number_t slot)
{
  struct forest          *forest   = vdo->block_map->forest;
  struct block_map_page  *page     = getTreePageFromForest(forest, root);
  struct data_location    mapping
    = vdo_unpack_block_map_entry(&page->entries[slot]);
  CU_ASSERT_FALSE(vdo_is_mapped_location(&mapping));
  CU_ASSERT_TRUE(vdo_is_valid_location(&mapping));
}

/**
 * Verify that corruptions have been removed from the block map tree.
 **/
static void verifyRebuiltMapAction(struct vdo_completion *completion)
{
  validateUnmappedEntry(2,  1);
  validateUnmappedEntry(4,  10);
  validateUnmappedEntry(6,  50);
  validateUnmappedEntry(8,  100);
  validateUnmappedEntry(10, VDO_BLOCK_MAP_ENTRIES_PER_PAGE / 2);
  validateUnmappedEntry(12, VDO_BLOCK_MAP_ENTRIES_PER_PAGE - 1);

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Verify that bad references in the block map tree are removed during
 * read-only rebuild. Certain tree entries are overwritten with bad references,
 * and read-only rebuild will remove those mappings to restore a consistency.
 **/
static void testBlockMapCorruption(void)
{
  block_count_t leafPages
    = vdo_compute_block_map_page_count(getTestConfig().config.logical_blocks);
  for (block_count_t i = 0; i < leafPages; i++) {
    writeData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, i, 1, VDO_SUCCESS);
  }

  const struct thread_config *threadConfig = vdo->thread_config;
  performSuccessfulActionOnThread(corruptMapAction,
                                  vdo_get_logical_zone_thread(threadConfig,
                                                              0));

  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RECOVERING);
  rebuildReadOnlyVDO();

  // Read all written blocks to make sure we can, and also to load the tree.
  char buffer[VDO_BLOCK_SIZE];
  for (block_count_t i = 0; i < leafPages; i++) {
    VDO_ASSERT_SUCCESS(performRead(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1,
                                   buffer));
  }

  threadConfig = vdo->thread_config;
  performSuccessfulActionOnThread(verifyRebuiltMapAction,
                                  vdo_get_logical_zone_thread(threadConfig,
                                                              0));
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test rebuild for block map tree corruption", testBlockMapCorruption },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "Block map tree rebuild tests (TreeRebuild_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
