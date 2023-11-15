/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "permassert.h"

#include "data-vio.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  MAPPABLE_BLOCKS = 64,
};

static bool              reachedPacker;
static struct data_vio  *toExamine;
static struct zoned_pbn  zpbn;

/**
 * Test-specific initialization.
 **/
static void initializeMootT1(void)
{
  TestParameters parameters = {
    .mappableBlocks      = MAPPABLE_BLOCKS,
    .logicalThreadCount  = 1,
    .physicalThreadCount = 1,
    .hashZoneThreadCount = 1,
    .enableCompression   = true,
  };
  initializeVDOTest(&parameters);
}

/**
 * Implements BlockCondition.
 **/
static bool
shouldBlock(struct vdo_completion *completion,
            void                  *context __attribute__((unused)))
{
  return lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION);
}

/**
 * Issue a write which will not get an allocation due to the VDO being full.
 * Block the write when it goes to query the index, and then issue a read for
 * the same lbn. Release the write and wait for the read and write to complete.
 *
 * @param lbn                  The logical block on which to operate
 * @param offset               The offset of the data to write
 * @param expectedWriteResult  The expected result of the write
 * @param buffer               The buffer to read into
 * @param hook                 A hook to set after the write has blocked,
 *                             may be NULL
 **/
static void launchWriteAndRead(logical_block_number_t  lbn,
                               block_count_t           offset,
                               int                     expectedWriteResult,
                               char                   *buffer,
                               CompletionHook         *hook)
{
  setBlockVIOCompletionEnqueueHook(shouldBlock, true);
  IORequest *request = launchIndexedWrite(lbn, 1, offset);
  waitForBlockedVIO();
  if (hook != NULL) {
    addCompletionEnqueueHook(hook);
  }

  IORequest *readRequest
    = launchBufferBackedRequest(lbn, 1, buffer, REQ_OP_READ);
  releaseBlockedVIO();
  CU_ASSERT_EQUAL(expectedWriteResult,
                  awaitAndFreeRequest(uds_forget(request)));
  awaitAndFreeSuccessfulRequest(readRequest);
}

/**
 * Implements VDOAction.
 **/
static void releaseLatchedVIO(struct vdo_completion *completion)
{
  clearCompletionEnqueueHooks();
  runSavedCallback(completion);
  struct data_vio *dataVIO = uds_forget(toExamine);
  reallyEnqueueCompletion(&dataVIO->vio.completion);
}

/**
 * Implements CompletionHook.
 **/
static bool wantsLogicalLockOnLBN1(struct vdo_completion *completion)
{
  if (logicalIs(completion, 1)) {
    wrapCompletionCallback(completion, releaseLatchedVIO);
  }

  return true;
}

/**
 * Test fulfilling reads and mooting of blocks in the compressor.
 **/
static void testReadFulfillmentAndCompressorMooting(void)
{
  setupCompressorLatch();

  // Write data at LBN 1.
  IORequest *request = launchIndexedWrite(1, 1, 1);

  // Wait for the VIO to land in the compressor and be trapped.
  waitForVIOLatchesAtCompressor();
  tearDownCompressorLatch();

  // Read the data from the VIO that is in the compressor.
  verifyData(1, 1, 1);
  toExamine = vio_as_data_vio(getBlockedVIO());
  CU_ASSERT_EQUAL(get_data_vio_compression_status(toExamine).stage,
                  DATA_VIO_COMPRESSING);

  // Prevent any more VIOs from going to the packer.
  preventPacking();

  /*
   * Once the next VIO is blocked waiting for the logical lock from the
   * previous VIO, release the previous VIO and ensure that any subsequent
   * VIO does not go to the packer.
   */
  setCompletionEnqueueHook(wantsLogicalLockOnLBN1);

  /*
   * Overwrite block 1 so as to moot the first VIO.
   */
  IORequest *request2 = launchIndexedWrite(1, 1, 2);

  // Wait for the initial write VIO to finish.
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  // Wait for the second write to finish.
  awaitAndFreeSuccessfulRequest(uds_forget(request2));
  restorePacking();

  // Verify that the overwrite happened
  verifyData(1, 2, 1);

  // Make sure compression was properly cancelled on the first VIO.
  CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, lookupLBN(1).state);
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_allocated(vdo), 1);

  // The packer should have been skipped by the original VIO, since it was
  // mooted in the compressor.
  struct packer_statistics stats = vdo_get_packer_statistics(vdo->packer);
  CU_ASSERT_EQUAL(0, stats.compressed_fragments_written);
}

/**
 * Check that the data_vio which has just arrived at the packer will be
 * packing.
 *
 * Implements vdo_action_fn.
 **/
static void assertPacking(struct vdo_completion *completion)
{
  toExamine = as_data_vio(completion);
  runSavedCallbackAssertNoRequeue(completion);
  CU_ASSERT_EQUAL(get_data_vio_compression_status(toExamine).stage,
                  DATA_VIO_PACKING);
  signalState(&reachedPacker);
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfLeavingCompressor(struct vdo_completion *completion)
{
  if (isLeavingCompressor(completion)) {
    wrapCompletionCallback(completion, assertPacking);
  }

  return true;
}

/**
 * Hook to check that the data vio recorded in assertPacking() has had its
 * compression canceled.
 *
 * Implements CompletionHook.
 **/
static bool assertCanceled(struct vdo_completion *completion)
{
  if (completion == &toExamine->vio.completion) {
    struct data_vio *dataVIO = uds_forget(toExamine);
    clearCompletionEnqueueHooks();
    CU_ASSERT_TRUE(get_data_vio_compression_status(dataVIO).may_not_compress);
  }

  return true;
}


/**
 * Check that a read was from the expected PBN.
 *
 * Implements CompletionHook
 **/
static bool checkPBN(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_CLEANUP)) {
    CU_ASSERT_EQUAL(zpbn.pbn, pbnFromVIO(as_vio(completion)));
    clearCompletionEnqueueHooks();
  }

  return true;
}

/**
 * Test fulfilling reads and mooting of blocks in the packer.
 **/
static void testReadFulfillmentAndPackerMooting(void)
{
  reachedPacker = false;
  setCompletionEnqueueHook(wrapIfLeavingCompressor);

  // Write data at LBN 1.
  IORequest *request = launchIndexedWrite(1, 1, 1);

  // Wait for the write to get into the packer.
  waitForState(&reachedPacker);

  // Read the data from the VIO that is in the packer.
  verifyData(1, 1, 1);

  // Make all subsequent VIOs skip the packer (so they finish).
  preventPacking();
  setCompletionEnqueueHook(assertCanceled);

  // Overwrite block 1 so as to moot the first VIO.
  writeAndVerifyData(1, 2, 1, getPhysicalBlocksFree(),
                     vdo_get_physical_blocks_allocated(vdo));

  /*
   * Fill the VDO with blocks that won't compress. This should hit the case
   * where a mooted VIO still holds a write lock on a physical block with
   * reference count 0 (VDO-2028).
   */
  block_count_t blocksToWrite = getPhysicalBlocksFree();
  writeAndVerifyData(2, 3, blocksToWrite, 0,
                     vdo_get_physical_blocks_allocated(vdo) + blocksToWrite);
  restorePacking();
  requestFlushPacker();

  // Wait for the initial write VIO to finish.
  awaitAndFreeSuccessfulRequest(request);

  // Make sure compression was properly cancelled.
  zpbn = lookupLBN(1);
  CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, zpbn.state);
  CU_ASSERT_EQUAL(vdo_get_physical_blocks_allocated(vdo), 1 + blocksToWrite);

  // Make sure the VDO is full
  block_count_t freeBlocks = getPhysicalBlocksFree();
  if (freeBlocks > 0) {
    performSetVDOCompressing(false);
    writeAndVerifyData(100, 80 - freeBlocks, freeBlocks, 0, 64);
    performSetVDOCompressing(true);
  }

  // Attempt to write unique data which will fail due to lack of space,
  // and a concurrent read which should not be serviced from the write.
  char buffer[VDO_BLOCK_SIZE];
  memset(buffer, 1, VDO_BLOCK_SIZE);
  launchWriteAndRead(99, 80 - freeBlocks - 1, VDO_NO_SPACE, buffer, NULL);
  for (uint32_t i = 0; i < VDO_BLOCK_SIZE; i++) {
    CU_ASSERT_EQUAL(0, buffer[i]);
  }

  // Figure out which block was written to lbn 1
  VDO_ASSERT_SUCCESS(performRead(1, 1, buffer));

  // Write duplicate data and a concurrent read which should not be serviced
  // from the write
  char buffer2[VDO_BLOCK_SIZE];
  STATIC_ASSERT(sizeof(block_count_t) == sizeof(uint64_t));
  block_count_t offset;
  memcpy(&offset, buffer, sizeof(offset));
  launchWriteAndRead(99, offset, VDO_SUCCESS, buffer2, checkPBN);
  UDS_ASSERT_EQUAL_BYTES(buffer, buffer2, VDO_BLOCK_SIZE);
}

/**
 * Test an overwrite that doesn't get an allocation doesn't lose data.
 **/
static void testFullOverwriteMooting(void)
{
  setupPackerNotification();

  // Write blocks normally to fill all but one block of the VDO.
  performSetVDOCompressing(false);
  writeAndVerifyData(1, 0, MAPPABLE_BLOCKS, 1, MAPPABLE_BLOCKS - 1);
  performSetVDOCompressing(true);

  // Write data at LBN 0.
  IORequest *request = launchIndexedWrite(0, 1, MAPPABLE_BLOCKS + 1);

  // Wait for the write to get into the packer.
  waitForDataVIOToReachPacker();
  tearDownPackerNotification();

  // Make sure the VDO is full at this point.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);

  /*
   * Overwrite block 0 so as to moot the first VIO. There is no room for it
   * to allocate, so it will finish with VDO_NO_SPACE.
   */
  writeData(0, MAPPABLE_BLOCKS + 2, 1, VDO_NO_SPACE);

  // Kick the packer and wait for the initial write VIO to finish.
  requestFlushPacker();
  awaitAndFreeSuccessfulRequest(uds_forget(request));

  // Make sure we haven't lost any data.
  verifyData(0, MAPPABLE_BLOCKS + 1, 1);
  verifyData(1, 0, MAPPABLE_BLOCKS);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test read fulfillment and mooting a write in the compressor",
    testReadFulfillmentAndCompressorMooting },
  { "test read fulfillment and mooting a write in the packer",
    testReadFulfillmentAndPackerMooting },
  { "test failed overwrite and mooting a write in the packer",
    testFullOverwriteMooting },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name        = "Tests of read fulfillment and write mooting (Moot_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeMootT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
