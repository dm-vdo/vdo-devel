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

#include "block-allocator.h"
#include "packer.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static IORequest     *requests[VDO_MAX_COMPRESSION_SLOTS];
static block_count_t  blocksFree;
static bool           finishedCompressedWrite;

/**
 * Test-specific initialization.
 **/
static void initializeCompressionT1(void)
{
  const TestParameters parameters = {
    .mappableBlocks      = 64,
    .logicalBlocks       = 64 * 3,
    .journalBlocks       = 32,
    .logicalThreadCount  = 1,
    .physicalThreadCount = 1,
    .hashZoneThreadCount = 1,
    .enableCompression   = true,
  };
  initializeVDOTest(&parameters);
  finishedCompressedWrite = false;
  blocksFree              = computeDataBlocksToFill();
}

/**
 * Test writing data into a VDO with compression enabled.
 **/
static void testCompressedDataReadWrite(void)
{
  block_count_t binCount      = blocksFree / VDO_MAX_COMPRESSION_SLOTS;
  block_count_t writeCount    = VDO_MAX_COMPRESSION_SLOTS * binCount;
  block_count_t blocksWritten = writeCount;

  // Write compressible blocks, which fit into multiple blocks.
  writeData(0, 1, blocksWritten, VDO_SUCCESS);
  uint64_t freeExpected = blocksFree - binCount;
  CU_ASSERT_EQUAL(freeExpected, getPhysicalBlocksFree());
  verifyData(0, 1, blocksWritten);

  // Write duplicate data.
  writeData(blocksWritten, 1, writeCount, VDO_SUCCESS);
  CU_ASSERT_EQUAL(freeExpected, getPhysicalBlocksFree());
  verifyData(blocksWritten, 1, writeCount);
  blocksWritten += writeCount;

  // Write another copy of the duplicate data.
  writeData(blocksWritten, 1, writeCount, VDO_SUCCESS);
  CU_ASSERT_EQUAL(freeExpected, getPhysicalBlocksFree());
  verifyData(blocksWritten, 1, writeCount);
  blocksWritten += writeCount;

  // Erase all references by writing zero blocks.
  for (block_count_t lbn = 0; lbn < blocksWritten; lbn++) {
    writeData(lbn, 0, 1, VDO_SUCCESS);
  }

  CU_ASSERT_EQUAL(blocksFree, getPhysicalBlocksFree());
}

/**
 * Test that writes which duplicate blocks that are waiting in the packer.
 **/
static void testDedupeBlocksInPacker(void)
{
  setupPackerNotification();
  // Write a compressible block.
  IORequest *request = launchIndexedWrite(2, 1, 1);

  // Wait for the VIO to enter the compression path.
  waitForDataVIOToReachPacker();
  tearDownPackerNotification();

  // Write a duplicate.
  writeData(3, 1, 1, VDO_SUCCESS);

  // Flush the packer.
  requestFlushPacker();

  // Wait for the initial write VIO to come back from the packer.
  awaitAndFreeSuccessfulRequest(UDS_FORGET(request));
  CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, lookupLBN(2).state);

  // Make sure the blocks deduplicated.
  verifyWrite(2, 1, 1, blocksFree - 1, 1);
  verifyWrite(3, 1, 1, blocksFree - 1, 1);
}

/**
 * Check whether a bio is doing a compressed block write.
 *
 * <p>Implements BlockCondition
 **/
static bool isCompressedWrite(struct vdo_completion *completion,
                              void *context __attribute__((unused)))
{
  if (!isDataVIO(completion)) {
    return false;
  }

  struct data_vio *data_vio = as_data_vio(completion);
  return ((data_vio->compression.slot == 0)
          && (data_vio->compression.next_in_batch != NULL));
}

/**
 * Issue requests to force a compressed block write.
 **/
static void fillCompressedBlock(void)
{
  // Fills a bin with small fragments so that a compressed block will be
  // written.
  for (int i = 0; i < VDO_MAX_COMPRESSION_SLOTS; i++) {
    requests[i] = launchIndexedWrite(i, 1, 1 + i);
  }
}

/**
 * Issue requests to force a compressed block write and wait for that
 * condition. Verify that a block is allocated for the compressed block and it
 * is singly referenced.
 **/
static void setupCompressBlockWriteAndWait(void)
{
  setBlockBIO(isCompressedWrite, true, true);
  fillCompressedBlock();
  waitForBlockedVIO();

  /*
   * Each one of the data_vios in the compressed block should have an
   * allocation. Since we no longer use compressed write vios, but rather, use
   * the allocation of one of the data_vios, there should not be an extra
   * allocation.
   */
  CU_ASSERT_EQUAL(blocksFree - VDO_MAX_COMPRESSION_SLOTS,
                  getPhysicalBlocksFree());
}

/**
 * Wait for each request to complete, asserting that the write was compressed
 * (or not) before freeing it.
 *
 * @param assertCompressed  If true, assert that each write was mapped
 *                          to a compressed block, otherwise assert that
 *                          each was not compressed
 **/
static void awaitRequests(bool assertCompressed)
{
  for (int i = 0; i < VDO_MAX_COMPRESSION_SLOTS; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
    enum block_mapping_state mappingState = lookupLBN(i).state;
    if (assertCompressed) {
      CU_ASSERT_TRUE(mappingState >= VDO_MAPPING_STATE_COMPRESSED_BASE);
    } else {
      CU_ASSERT_EQUAL(mappingState, VDO_MAPPING_STATE_UNCOMPRESSED);
    }
  }
}

/**
 * Test that VDO maintains the reference state for a block that is allocated
 * for a compressed block.
 **/
static void testCompressedBlockReference(void)
{
  setupCompressBlockWriteAndWait();

  // Release the blocked compressed block write to allow pending writes to
  // finish.
  releaseBlockedVIO();
  awaitRequests(true);
  CU_ASSERT_EQUAL(blocksFree - 1, getPhysicalBlocksFree());
  // XXX assert refCount of blockedPBN?
}

/**
 * Implements VDOAction.
 **/
static void releaseAfterHashZone(struct vdo_completion *completion)
{
  clearCompletionEnqueueHooks();
  runSavedCallbackAssertNoRequeue(completion);
  releaseVIOLatchedInCompressor();
}

/**
 * Impelements CompletionHook
 **/
static bool wrapIfEnteringHashZone(struct vdo_completion *completion)
{
  if (logicalIs(completion, 3)
      && (completion->callback_thread_id
          == vdo->thread_config->hash_zone_threads[0])) {
    wrapCompletionCallback(completion, releaseAfterHashZone);
  }

  return true;
}

/**
 * Test that writes which duplicate blocks that are in the compressor don't
 * block indefinitely.
 **/
static void testDedupeBlocksInCompressor(void)
{
  setupCompressorLatch();

  // Write a compressible block.
  IORequest *request = launchIndexedWrite(2, 1, 1);

  // Wait for the VIO to enter the compression path.
  waitForVIOLatchesAtCompressor();

  // Write a duplicate.
  setCompletionEnqueueHook(wrapIfEnteringHashZone);
  writeData(3, 1, 1, VDO_SUCCESS);

  // Wait for the initial write VIO to come back from the packer.
  awaitAndFreeSuccessfulRequest(UDS_FORGET(request));

  // Make sure it got cancelled out from the compression path.
  CU_ASSERT_EQUAL(lookupLBN(2).state, VDO_MAPPING_STATE_UNCOMPRESSED);

  // Make sure the blocks deduplicated.
  CU_ASSERT_EQUAL(blocksFree - 1, getPhysicalBlocksFree());
  verifyData(2, 1, 1);
  verifyData(3, 1, 1);
}

/**********************************************************************/
static void writeCompressableData(block_count_t blocks,
                                  block_count_t offset,
                                  IORequest *requests[])
{
  setupPackerNotification();
  for (block_count_t i = 0; i < blocks; i++) {
    requests[i] = launchIndexedWrite(i, 1, i + offset);
    waitForDataVIOToReachPacker();
  }
  tearDownPackerNotification();
}

/**********************************************************************/
static void testReadOnlyModeWithBlocksInPacker(void)
{
  block_count_t REQUEST_COUNT = 2;
  writeCompressableData(REQUEST_COUNT, 1, requests);
  requestFlushPacker();
  for (block_count_t i = 0; i < REQUEST_COUNT; i++) {
    awaitAndFreeRequest(UDS_FORGET(requests[i]));
  }

  writeCompressableData(REQUEST_COUNT, 1 + REQUEST_COUNT, requests);
  forceVDOReadOnlyMode();
  requestFlushPacker();
  for (block_count_t i = 0; i < 2; i++) {
    awaitAndFreeRequest(UDS_FORGET(requests[i]));
  }
}

/**********************************************************************/
static void testReadOnlyModeWithBlocksInPackerNoHang(void)
{
  // Turn off assertion failures in the base code so that even if the proximal
  // assertion failure in VDO-2456 fires, the test won't abort.
  set_exit_on_assertion_failure(false);
  testReadOnlyModeWithBlocksInPacker();
  set_exit_on_assertion_failure(true);
}

/**********************************************************************/
static void testReadOnlyModeWithBlocksInPackerNoAssert(void)
{
  // Test that the proximal assertion failure in VDO-2456 have been fixed.
  testReadOnlyModeWithBlocksInPacker();
}

/**
 * Test that reading a damaged or invalid compressed block returns an I/O
 * error and does not put the VDO into read-only mode. Data blocks can be
 * corrupted or we can crash in async mode, leaving a block map entry for a
 * compressed block pointing at an physical block containing arbitrary data.
 **/
static void testInvalidFragment(void)
{
  setupCompressBlockWriteAndWait();
  struct vio              *compressedWriteVIO = getBlockedVIO();
  physical_block_number_t  compressedPhysical = compressedWriteVIO->physical;
  CU_ASSERT_NOT_EQUAL(0, compressedPhysical);
  reallyEnqueueBIO(compressedWriteVIO->bio);

  // Wait for all the fragment writes to complete.
  awaitRequests(true);

  // Check that we can read all the fragments.
  verifyData(0, 1, VDO_MAX_COMPRESSION_SLOTS);

  // Smash the compressed block.
  PhysicalLayer *syncLayer = getSynchronousLayer();
  char *buffer;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(VDO_BLOCK_SIZE, char, __func__, &buffer));
  VDO_ASSERT_SUCCESS(syncLayer->writer(syncLayer, compressedPhysical, 1,
                                       buffer));

  /*
   * Attempt to read one of the compressed fragments. We should see the
   * invalid fragment error (which would be translated to EIO); the VDO should
   * not be put into read-only mode.
   */
  CU_ASSERT_EQUAL(VDO_INVALID_FRAGMENT, performRead(0, 1, buffer));
  CU_ASSERT_FALSE(vdo_in_read_only_mode(vdo));
  UDS_FREE(buffer);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "compressed data read write",        testCompressedDataReadWrite         },
  { "dedupe block in packer",            testDedupeBlocksInPacker            },
  { "dedupe block in compressor",        testDedupeBlocksInCompressor        },
  { "compressed block reference",        testCompressedBlockReference        },
  { "test entering read-only mode with blocks in the packer doesn't hang",
                                  testReadOnlyModeWithBlocksInPackerNoHang   },
  { "test entering read-only mode with blocks in the packer doesn't assert",
                                  testReadOnlyModeWithBlocksInPackerNoAssert },
  { "handling of invalid fragment errors", testInvalidFragment               },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "VDO Compression test (Compression_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeCompressionT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
