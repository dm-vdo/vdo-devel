/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "admin-state.h"
#include "block-map.h"
#include "device-config.h"
#include "types.h"
#include "vdo-component-states.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "asyncVIO.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct vio     *pageCacheWrite;
static struct vio     *treePageWrite;
static bool            blocked;
static bool            draining;
static bool            writeComplete;
static thread_id_t     logicalZoneThread;

/**
 * Initialize the test.
 **/
static void initialize(void)
{
  TestParameters parameters = {
    .mappableBlocks    = 1024,
    // We want to use the first two leaves of the first tree.
    .logicalBlocks     = ((DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT + 1)
                          * VDO_BLOCK_MAP_ENTRIES_PER_PAGE),
    // Make sure there is only one logical zone
    .logicalThreadCount = 1,
    .dataFormatter      = fillWithOffsetPlusOne,
    // Make sure the era length is such that every tree page isn't already
    // expired when dirtied.
    .journalBlocks      = 16,
  };

  initializeVDOTest(&parameters);

  // Make sure the first tree is allocated down to the first leaf.
  writeData(0, 0, 1, VDO_SUCCESS);

  // Restart the VDO so that the pages are all written and the rest of the test
  // won't block if we trap writes.
  restartVDO(false);

  logicalZoneThread = vdo_get_logical_zone_thread(vdo->thread_config, 0);
}

/**
 * An action to check the state of a block map zone before running the saved
 * callback from the released block map write.
 *
 * Implements VDOAction.
 **/
static void checkBlockMapState(struct vdo_completion *completion)
{
  struct block_map *blockMap = vdo->block_map;
  CU_ASSERT(vdo_is_state_draining(&blockMap->zones[0].state));
  runSavedCallback(completion);
  signalState(&writeComplete);
}

/**
 * Wrap the callbacks when either of the trapped writes are released.
 *
 * Implements CompletionHook
 **/
static bool wrapPreviouslyTrapped(struct vdo_completion *completion)
{
  if ((treePageWrite != NULL) && (completion == &treePageWrite->completion)) {
    treePageWrite = NULL;
    wrapVIOCallback(as_vio(completion), checkBlockMapState);
  } else if ((pageCacheWrite != NULL)
             && (completion == &pageCacheWrite->completion)) {
    pageCacheWrite = NULL;
    wrapVIOCallback(as_vio(completion), checkBlockMapState);
  }

  return true;
}

/**
 * Trap one page cache write and one tree page write.
 *
 * Implements BIOSubmitHook.
 **/
static bool trapBlockMapWrites(struct bio *bio)
{
  if (bio_op(bio) != REQ_OP_WRITE) {
    return true;
  }

  struct vio *vio = bio->bi_private;
  if (vio->type == VIO_TYPE_BLOCK_MAP_INTERIOR) {
    if (treePageWrite != NULL) {
      return true;
    }

    struct block_map_page *page = (struct block_map_page *) vio->data;
    if (!page->header.initialized) {
      return true;
    }

    treePageWrite = vio;
  } else if (vio->type == VIO_TYPE_BLOCK_MAP) {
    if (pageCacheWrite != NULL) {
      return true;
    }

    struct block_map_page *page = (struct block_map_page *) vio->data;
    if (!page->header.initialized) {
      return true;
    }

    pageCacheWrite = vio;
  } else {
    return true;
  }

  if ((treePageWrite != NULL) && (pageCacheWrite != NULL)) {
    clearBIOSubmitHook();
    signalState(&blocked);
  }

  return false;
}

/**
 * An action to advance the block map era.
 *
 * Implements VDOAction.
 **/
static void advanceEra(struct vdo_completion *completion)
{
  struct device_config config = getTestConfig().deviceConfig;
  struct block_map *blockMap = vdo->block_map;
  vdo_advance_block_map_era(blockMap,
                            blockMap->current_era_point
                            + config.block_map_maximum_age);
  vdo_complete_completion(completion);
}

/**
 * Check whether the single block map zone is draining.
 *
 * Implements FinishHook.
 **/
static void checkDraining(void)
{
  if (vdo_get_callback_thread_id() != logicalZoneThread) {
    return;
  }

  if (vdo_is_state_draining(&(vdo->block_map->zones[0].state))) {
    signalState(&draining);
  }
}

/**
 * Test that the block map does not prematurely decide it has drained due to
 * an outstanding write.
 *
 * @param drainType  The type of drain to perform (suspend or save)
 * @param treeFirst  If <code>true</code>, release the tree page write first,
 *                   otherwise release the page cache write first.
 **/
static void testDrainWithBlockedWrite(const struct admin_state_code *drainType,
                                      bool                           treeFirst)
{
  // Prepare to trap block map writes
  clearState(&blocked);
  pageCacheWrite = NULL;
  treePageWrite = NULL;
  setBIOSubmitHook(trapBlockMapWrites);

  // Write a block to the second leaf of the first tree.
  writeData(DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT
              * VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
            1, 1, VDO_SUCCESS);

  // Advance the block map era so that everything will be written out.
  performSuccessfulAction(advanceEra);
  waitForState(&blocked);

  // Start draining
  clearState(&draining);
  setCallbackFinishedHook(checkDraining);
  struct vdo_completion *completion = launchBlockMapAction(vdo->block_map,
                                                           drainType);
  waitForState(&draining);

  /*
   * Now that we know we are draining, release the a write. If we have fixed
   * [VDO-4800], this will not result in an early notification that the drain
   * is complete.
   */
  clearState(&writeComplete);
  setCompletionEnqueueHook(wrapPreviouslyTrapped);
  reallyEnqueueBIO(treeFirst ? treePageWrite->bio : pageCacheWrite->bio);
  waitForState(&writeComplete);

  // Now release the tree write. If we have fixed the bug, the zone will still
  // be suspending.
  reallyEnqueueBIO(treeFirst ? pageCacheWrite->bio : treePageWrite->bio);

  // Wait for the drain to complete
  awaitCompletion(completion);
  UDS_FREE(completion);

  // Resume the block map so that teardown succeeds.
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RESUMING);
}

/**
 * Test suspend with an outstanding tree page write.
 **/
static void testSuspendTreeFirst(void)
{
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SUSPENDING, true);
}

/**
 * Test save with an outstanding tree page write.
 **/
static void testSaveTreeFirst(void)
{
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SAVING, true);
}

/**
 * Test suspend with an outstanding page cache write.
 **/
static void testSuspendCacheFirst(void)
{
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SUSPENDING, false);
}

/**
 * Test save with an outstanding page cache write.
 **/
static void testSaveCacheFirst(void)
{
  testDrainWithBlockedWrite(VDO_ADMIN_STATE_SAVING, false);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test block map suspend tree drains first",  testSuspendTreeFirst,    },
  { "test block map suspend cache drains first", testSuspendCacheFirst,   },
  { "test block map save tree drains first",     testSaveTreeFirst,       },
  { "test block map save cache drains first",    testSaveCacheFirst,      },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "test block map drain [VDO-4800] (BlockMapDrain_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
