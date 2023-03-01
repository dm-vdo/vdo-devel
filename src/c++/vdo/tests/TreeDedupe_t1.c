/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "block-map.h"
#include "vdo-component-states.h"
#include "volume-geometry.h"

#include "vdoConfig.h"

#include "ioRequest.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

block_count_t blocksWritten;
physical_block_number_t treeBlock;

/**
 * Initialize the test.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks      = 16,
    .slabSize            = 32,
    .logicalBlocks       = VDO_BLOCK_MAP_ENTRIES_PER_PAGE + 1,
    .logicalThreadCount  = 1,
    .physicalThreadCount = 1,
    .hashZoneThreadCount = 1,
    .dataFormatter       = fillWithOffsetPlusOne,
  };
  initializeVDOTest(&parameters);

  /*
   * Fill the VDO so that there is data in all blocks, then trim enough
   * space to do one more write including allocation of an entire new
   * block map tree.
   */
  blocksWritten = fillPhysicalSpace(0, 0);
  VDO_ASSERT_SUCCESS(performTrim(5, 5));
}

/**********************************************************************/
static void verify(void)
{
  /*
   * The leaf block map page for the failed write should have been allocated
   * but not written. Confirm that the contents of that block are not a
   * block map page.
   */
  char buffer[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, treeBlock, 1, buffer));

  // Confirm that the on-disk contents are not a block map page.
  struct block_map_page *blockMapPage = (struct block_map_page *) buffer;
  nonce_t nonce = vdo->geometry.nonce;
  CU_ASSERT_EQUAL(VDO_BLOCK_MAP_PAGE_INVALID,
		  vdo_validate_block_map_page(blockMapPage,
					      nonce,
					      treeBlock));

  waitForRecoveryDone();

  // There should be one block free since we filled, trimmed 5 blocks,
  // allocated 4 block map pages, and failed one data write.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);

  // Write new data to the one unallocated block
  writeData(blocksWritten, blocksWritten, 1, VDO_SUCCESS);

  // There should now be no space
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);

  // Now attempt to write a duplicate of the data in the unwritten block
  // map page.
  CU_ASSERT_EQUAL(performWrite(blocksWritten + 1, 1, buffer), VDO_NO_SPACE);
}

/**
 * Check whether a completion is about to be enqueued for acknowledgement, and
 * if so, record the PBN of the leaf page and flush the RAM layer.
 *
 * Implements BlockCondition
 **/
static bool
checkForAcknowledgement(struct vdo_completion *completion,
                        void                  *context __attribute__((unused)))
{
  if (completion->callback_thread_id != vdo->thread_config->bio_ack_thread) {
    return false;
  }

  // Record the PBN of the leaf block map page which our failed write request
  // allocated.
  treeBlock
    = as_data_vio(completion)->tree_lock.tree_slots[0].block_map_slot.pbn;
  flushRAMLayer(getSynchronousLayer());
  return true;
}

/**
 * An enqueue hook which will flush and prepare to crash the RAM layer before
 * acknowledging a data_vio. This will prevent the data_vio from actually
 * writing its data after it has successfully allocated block map pages.
 *
 * <p>Implements CompletionHook
 **/
static bool prepareToCrashOnAcknowledgement(struct vdo_completion *completion)
{
  if (checkForAcknowledgement(completion, NULL)) {
    prepareToCrashRAMLayer(getSynchronousLayer());
    clearCompletionEnqueueHooks();
  }

  return true;
}

/**********************************************************************/
static void testNoDedupeAfterRecovery(void)
{
  setCompletionEnqueueHook(prepareToCrashOnAcknowledgement);
  VDO_ASSERT_SUCCESS(performIndexedWrite(VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
                                         1,
                                         blocksWritten));
  crashVDO();
  startVDO(VDO_DIRTY);
  verify();
}

/**********************************************************************/
static void testNoDedupeAfterRebuild(void)
{
  setBlockVIOCompletionEnqueueHook(checkForAcknowledgement, true);
  IORequest *request
    = launchIndexedWrite(VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, blocksWritten);
  waitForBlockedVIO();
  forceVDOReadOnlyMode();
  releaseBlockedVIO();
  awaitAndFreeRequest(UDS_FORGET(request));
  rebuildReadOnlyVDO();
  verify();
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test recovered block map page dedupe (VDO-3261)",
    testNoDedupeAfterRecovery },
  { "test rebuilt block map page dedupe (VDO-3261)",
    testNoDedupeAfterRebuild },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name = "test no dedupe of block map blocks (TreeDedupe_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
