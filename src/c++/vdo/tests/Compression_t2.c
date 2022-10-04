/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "data-vio.h"
#include "packer.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  REQUEST_COUNT = 5,
};

typedef struct {
  struct zoned_pbn mapping;
  bool             duplicate;
} Results;

static bool                     hookTriggered;
static physical_block_number_t  vio6Physical;
static physical_block_number_t  compressedBlock;
static Results                  results[REQUEST_COUNT];

/**
 * Test-specific initialization.
 **/
static void initializeCompressionT2(void)
{
  hookTriggered = false;
  vio6Physical  = VDO_ZERO_BLOCK;

  TestParameters parameters = {
    .mappableBlocks    = 64,
    .enableCompression = true,
  };
  initializeVDOTest(&parameters);
}

/**
 * Implements CompletionHook.
 **/
static bool releaseBlockedVIOHook(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    return true;
  }

  clearCompletionEnqueueHooks();

  // Enqueue the second VIO, then enqueue the first VIO, so that the second
  // VIO verifies its advice before the first VIO can enter the packer.
  reallyEnqueueCompletion(completion);
  releaseBlockedVIO();
  return false;
}

/**
 * Implements CompletionHook.
 **/
static bool blockFirstVIO(struct vdo_completion *completion) {
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    return true;
  }

  setCompletionEnqueueHook(releaseBlockedVIOHook);
  blockVIO(as_vio(completion));
  return false;
}

/**
 * Test dedupe against a block which has updated UDS but hasn't yet
 * gone to the packer.
 **/
static void testDedupeVsPreCompressorVIO(void)
{
  block_count_t freeBlocks = populateBlockMapTree();
  setCompletionEnqueueHook(blockFirstVIO);

  // Write data at LBN 1.
  IORequest *firstRequest = launchIndexedWrite(1, 1, 1);

  // Wait for the write to block after the UDS query.
  waitForBlockedVIO();

  // Write the data again at LBN 2.
  writeData(2, 1, 1, VDO_SUCCESS);
  verifyData(2, 1, 1);

  // Wait for the first VIO to come back, having not entered the packer.
  awaitAndFreeSuccessfulRequest(firstRequest);

  // Make sure it didn't get compressed.
  CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, lookupLBN(1).state);
  verifyData(1, 1, 1);
  // We expect that the extraneously-written block will be immediately freed
  // when both VIOs are completed.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), freeBlocks - 1);
}

/**
 * A hook to record some of the state of each data_vio as it is about to clean
 * up.
 *
 * Implements vdo_action
 **/
static bool recordHook(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_CLEANUP)) {
    struct data_vio *dataVIO = as_data_vio(completion);
    results[dataVIO->logical.lbn] = (Results) {
      .mapping   = dataVIO->new_mapped,
      .duplicate = dataVIO->is_duplicate,
    };
  }

  return true;
}

/**********************************************************************/
static void giveVIO7StaleAdvice(struct vdo_completion *completion)
{
  // Pretend that this VIO got stale advice to test the convoluted
  // advice case.
  struct zoned_pbn staleAdvice = (struct zoned_pbn) {
    .pbn   = vio6Physical,
    .state = VDO_MAPPING_STATE_UNCOMPRESSED,
  };

  VDO_ASSERT_SUCCESS(vdo_get_physical_zone(vdo,
                                           vio6Physical,
                                           &staleAdvice.zone));
  set_data_vio_duplicate_location(as_data_vio(completion), staleAdvice);
  // XXX this is the pre-VDOSTORY-190 dedupe entry point
  // verifyAdvice(completion);
}

/**
 * Make a VIO go through giveVIO7StaleAdvice.
 *
 * Implements CompletionHook.
 */
static bool redirectVIO(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    return recordHook(completion);
  }

  CU_ASSERT_TRUE(as_data_vio(completion)->is_duplicate);
  completion->callback = giveVIO7StaleAdvice;
  setupPackerNotification();
  return true;
}

/**
 * Implements CompletionHook.
 *
 * Notify on VIOs 5 and 6 hitting deduplication.
 **/
static bool notifyOnVIOs5and6(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    return recordHook(completion);
  }

  struct data_vio *data_vio = as_data_vio(completion);
  CU_ASSERT_TRUE(data_vio->is_duplicate);
  if (logicalIs(completion, 6)) {
    vio6Physical = data_vio->new_mapped.pbn;
  }

  setCompletionEnqueueHook(recordHook);
  signalState(&hookTriggered);
  return true;
}

/**
 * Implements CompletionHook.
 **/
static bool trapVIO4(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_UPDATE_DEDUPE_INDEX)
      && logicalIs(completion, 4)) {
    blockVIO(as_vio(completion));
    return false;
  }

  return true;
}

/**
 * Test dedupe against blocks which have been compressed but not
 * yet updated UDS.
 **/
static void testDedupeVsPostPackingVIO(void)
{
  IORequest *requests[REQUEST_COUNT];

  // Set the number of slots in a compressed block to 2 so that we don't
  // need to explicitly flush the packer.
  /*
   * XXX: This is the only use of this function. Eliminating it allows some
   *      simplifications of types and the packer itself. If this test is
   *      ever resurrected, we should be able to replace the use of this
   *      function by either pre-writing 12 other blocks so that the two
   *      in the test will fill a packer bin, or by generating data in the
   *      two blocks we care about which compress to fill a bin.
   */
#ifdef DISABLED
  vdo_reset_packer_slot_count(vdo->packer, 2);
#endif // DISABLED

  // Set up to record the new_mapped fields of each data_vio as it completes.
  // Every other completion enqueue hook used by this test will also call this
  // hook.
  setCompletionEnqueueHook(recordHook);

  /*
   * Write two blocks at logical addresses 3 & 4 which will both compress (we
   * need two blocks in order to actually write a compressed block). Block VIO
   * 4 before it updates UDS with its compressed location.
   */
  addCompletionEnqueueHook(trapVIO4);
  requests[0] = launchIndexedWrite(3, 1, 3);
  requests[1] = launchIndexedWrite(4, 1, 4);
  waitForBlockedVIO();

  hookTriggered = false;
  setCompletionEnqueueHook(notifyOnVIOs5and6);
  // Write a copy of LBN 4's data at LBN 5. This tests waiting for a compressed
  // block holding a PBN write lock.
  requests[2] = launchIndexedWrite(5, 1, 4);
  waitForStateAndClear(&hookTriggered);

  setCompletionEnqueueHook(notifyOnVIOs5and6);
  // Write another copy of LBN 4's data at LBN 6. This tests waiting for a
  // compressed block (LBN 4) holding a PBN read lock.
  requests[3] = launchIndexedWrite(6, 1, 4);
  waitForState(&hookTriggered);

  /*
   * Write another duplicate of the data at 4 at 7. However, with this block
   * we will simulate convoluted stale advice by rewriting the VIO's duplicate
   * field to point at the physical block PBN write locked by the write to
   * logical 6. This will test the case where a VIO is trying to dedupe against
   * a write lock holder who is also trying to dedupe and therefore the VIO
   * should give up on deduplicating.
   *
   * The first 4 blocks should get compressed and the blocks written to LBNs 5
   * and 6 should end up as duplicates of the second block.
   */
  setCompletionEnqueueHook(redirectVIO);
  requests[4] = launchIndexedWrite(7, 1, 4);
  waitForDataVIOToReachPacker();
  setCompletionEnqueueHook(recordHook);
  releaseBlockedVIO();
  requestFlushPacker();

  for (int i = 0; i < REQUEST_COUNT; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
    if (i < 4) {
      CU_ASSERT_EQUAL((i > 1), results[i].duplicate);
      if (i > 1) {
        CU_ASSERT_EQUAL(results[1].mapping.pbn, results[i].mapping.pbn);
        CU_ASSERT_EQUAL(results[1].mapping.state, results[i].mapping.state);
      }
    } else {
      CU_ASSERT_FALSE(vdo_is_state_compressed(results[i].mapping.state));
      CU_ASSERT_FALSE(results[i].duplicate);
    }

    verifyData(i + 3, (i == 0) ? 3 : 4, 1);
  }
}

/**
 * Implements BlockCondition.
 **/
static bool trapVIO0(struct vdo_completion *completion,
                     void *context __attribute__((unused)))
{
  if (logicalIs(completion, 0)
      && lastAsyncOperationIs(completion,
                              VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    CU_ASSERT_EQUAL(as_data_vio(completion)->new_mapped.pbn, compressedBlock);
    return true;
  }

  return false;
}

/**
 * Implements CompletionHook.
 **/
static bool releaseVIOAfterQuery(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)
      || !logicalIs(completion, 1)) {
    return true;
  }

  /*
   * We want to attempt to verify against the VIO we trapped and then release
   * that VIO, so enqueue the VIO we are now processing and then the trapped
   * VIO.
   */
  reallyEnqueueCompletion(completion);
  releaseBlockedVIO();
  return false;
}

/**
 * Test dedupe against a block which overwrites a compressed block containing
 * the same data.
 **/
static void testDedupeVsOverwrittenCompressedBlock(void)
{
  block_count_t mappableBlocks = populateBlockMapTree();

  /* Write two compressed blocks */
  const block_count_t REQUEST_COUNT = 2;
  IORequest *requests[REQUEST_COUNT];
  setupPackerNotification();
  for (unsigned int i = 0; i < REQUEST_COUNT; i++) {
    requests[i] = launchIndexedWrite(i, 1, mappableBlocks + 1 + i);
    waitForDataVIOToReachPacker();
  }

  tearDownPackerNotification();
  requestFlushPacker();

  for (unsigned int i = 0; i < REQUEST_COUNT; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
    struct zoned_pbn zoned = lookupLBN(i);
    CU_ASSERT_TRUE(vdo_is_state_compressed(zoned.state));
    compressedBlock = zoned.pbn;
  }

  /*
   * Fill the rest of the physical space.
   */
  performSetVDOCompressing(false);
  writeData(2, 1, mappableBlocks - 1, VDO_SUCCESS);
  performSetVDOCompressing(true);

  // Overwrite the two compressed blocks with 0 blocks to free the physical
  // block containing the compressed block.
  zeroData(0, 2, VDO_SUCCESS);

  /*
   * Write the data we originally wrote to logical block 0, but block it before
   * it queries UDS.
   */
  setBlockVIOCompletionEnqueueHook(trapVIO0, true);
  requests[0] = launchIndexedWrite(0, 1, mappableBlocks + 1);
  waitForBlockedVIO();

  /*
   * Write the data we originally wrote to logical block 0 again. UDS
   * should give us stale advice which points to the same physical block
   * as the one we have just written. It will have the same data, but the
   * mapping state will be wrong.
   */
  setCompletionEnqueueHook(releaseVIOAfterQuery);
  writeAndVerifyData(1, mappableBlocks + 1, 1, 0, mappableBlocks);
  awaitAndFreeRequest(UDS_FORGET(requests[0]));
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  CU_TEST_INFO_NULL,
  { "dedupe vs. post-packer VIO",  testDedupeVsPostPackingVIO    },
  // XXX VDOSTORY-190 changes the dedupe path, which breaks these, and will
  // continue to change it, so they're disabled until things stabilize.
  { "dedupe vs. pre-compress VIO", testDedupeVsPreCompressorVIO  },
  { "dedupe vs. compressed overwrite VIO",
    testDedupeVsOverwrittenCompressedBlock
  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name = "Tests of dedupe against blocks being compressed (Compression_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeCompressionT2,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
