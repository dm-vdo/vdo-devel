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

#include "admin-state.h"
#include "compression-state.h"
#include "hash-lock.h"
#include "packer.h"
#include "thread-config.h"
#include "vdo.h"
#include "wait-queue.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static block_count_t    expectedSlotsUsed;
static block_count_t    packedItemCount;
static block_count_t    targetItemCount;
static block_size_t     binSize;
static bool             packed;
static bool             shouldQueue;
static bool             allBinsFull;
static block_size_t     compressedSizes[64];

/**
 * Setup physical and asynchronous layer, then create a packer to use the
 * asynchronous layer.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks       = 64,
    .journalBlocks        = 8,
    .logicalThreadCount   = 1,
    .enableCompression    = true,
    .disableDeduplication = true,
    .dataFormatter        = fillWithOffsetPlusOne,
  };
  initializeVDOTest(&parameters);
  // Populate the block map tree to make expectations of the number of blocks
  // consumed by the packer easier to determine.
  populateBlockMapTree();
  binSize = vdo->packer->bin_data_size;
}

/**
 * This tests a bin can hold an expected number of items.
 **/
static void binBoundaryTest(void)
{
  block_count_t freeBlocks = getPhysicalBlocksFree();
  // A bin should be full when the 14th data_vio is added to it, this will
  // hang if that isn't enough to trigger a bin write.
  writeData(0, 1, 14, VDO_SUCCESS);
  // If all 14 fit, only one block will have been used.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), freeBlocks - 1);
}

/**
 * Set the compressed size on exit from the compressor.
 *
 * Implements vdo_action
 **/
static void setCompressedSize(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = as_data_vio(completion);
  dataVIO->compression.size = compressedSizes[dataVIO->logical.lbn];

  if (shouldQueue) {
    runSavedCallbackAssertRequeue(completion);
    return;
  }

  runSavedCallbackAssertNoRequeue(completion);
  if (++packedItemCount == targetItemCount) {
    signalState(&packed);
  }
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfLeavingCompressor(struct vdo_completion *completion)
{
  if (isLeavingCompressor(completion)) {
    wrapCompletionCallback(completion, setCompressedSize);
  }

  return true;
}

/**
 * Check that each bin contains exactly the expected number of data_vios.
 *
 * Implements vdo_action
 **/
static void checkBins(struct vdo_completion *completion)
{
  for (struct packer_bin *bin = vdo_get_packer_fullest_bin(vdo->packer);
       bin != NULL;
       bin = vdo_next_packer_bin(vdo->packer, bin)) {
    CU_ASSERT_EQUAL(bin->slots_used, expectedSlotsUsed);
  }

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * This tests packing a sequence of items in bins and ensures the list
 * is organized for best-fit bin packing.
 **/
static void bestFitTest(void)
{
  block_count_t freeBlocks = getPhysicalBlocksFree();

  struct packer_statistics stats = vdo_get_packer_statistics(vdo->packer);
  CU_ASSERT_EQUAL(0, stats.compressed_fragments_written);
  CU_ASSERT_EQUAL(0, stats.compressed_blocks_written);

  // Add an item to each bin.
  IORequest *requests[DEFAULT_PACKER_BINS + 1];

  packedItemCount = 0;
  targetItemCount = DEFAULT_PACKER_BINS;
  packed = false;
  shouldQueue = false;
  setCompletionEnqueueHook(wrapIfLeavingCompressor);
  for (block_size_t i = 1; i <= DEFAULT_PACKER_BINS; i++) {
    // For the first batch, set the compressed size of each data_vio to nearly
    // fill a bin and be unique.
    compressedSizes[i] = binSize - i;
    requests[i] = launchIndexedWrite(i, 1, i);
  }

  waitForState(&packed);
  stats = vdo_get_packer_statistics(vdo->packer);
  CU_ASSERT_EQUAL(stats.compressed_fragments_in_packer, DEFAULT_PACKER_BINS);

  // Each bin should contain exactly one vio.
  expectedSlotsUsed = 1;
  performSuccessfulActionOnThread(checkBins,
                                  vdo->thread_config->packer_thread);

  // Add an item that would fit exactly in one of the unused bins in reverse
  // order.
  shouldQueue = true;
  for (block_size_t i = 2 * DEFAULT_PACKER_BINS;
       i > DEFAULT_PACKER_BINS;
       i--) {
    // For the second batch, set the compressed size to exactly fill the
    // emptiest non-empty bin.
    compressedSizes[i] = i - DEFAULT_PACKER_BINS;
    writeData(i, i + 1, 1, VDO_SUCCESS);
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i - DEFAULT_PACKER_BINS]));
  }

  stats = vdo_get_packer_statistics(vdo->packer);
  CU_ASSERT_EQUAL(2 * DEFAULT_PACKER_BINS, stats.compressed_fragments_written);
  CU_ASSERT_EQUAL(DEFAULT_PACKER_BINS, stats.compressed_blocks_written);
  CU_ASSERT_EQUAL(stats.compressed_fragments_in_packer, 0);

  // Each bin should be empty.
  expectedSlotsUsed = 0;
  performSuccessfulActionOnThread(checkBins,
                                  vdo->thread_config->packer_thread);

  // We should have written exactly 1 block per bin.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), freeBlocks - DEFAULT_PACKER_BINS);
}

/**
 * Test suspend and resume of an empty packer.
 **/
static void suspendEmptyPackerTest(void)
{
  performSuccessfulPackerAction(VDO_ADMIN_STATE_SUSPENDING);
  performSuccessfulPackerAction(VDO_ADMIN_STATE_RESUMING);
  performSuccessfulPackerAction(VDO_ADMIN_STATE_SUSPENDING);
  performSuccessfulPackerAction(VDO_ADMIN_STATE_RESUMING);
}

/**
 * Signal when all the bins are full.
 *
 * Implements VDOAction
 **/
static void signalAllBinsFull(struct vdo_completion *completion)
{
  runSavedCallbackAssertNoRequeue(completion);
  if (++packedItemCount == (DEFAULT_PACKER_BINS * 2)) {
    signalState(&allBinsFull);
  }
}

/**
 * Check for a data_vio on its way to the packer.
 *
 * Implements CompletionHook
 **/
static bool wrapIfHeadingToPacker(struct vdo_completion *completion)
{
  if ((completion->callback_thread_id == vdo->thread_config->packer_thread)
      && lastAsyncOperationIs(completion, VIO_ASYNC_OP_COMPRESS_DATA_VIO)) {
    /*
     * Set the compressed size such that each bin will receive two data vios
     * which don't fill the bin, but don't leave room for a third. This ensures
     * that all the bins will be full but that none will write out.
     */
    as_data_vio(completion)->compression.size = (binSize - 10) / 2;
    wrapCompletionCallback(completion, signalAllBinsFull);
  }

  return true;
}

/**
 * Test that the packer may be suspended and resumed, and that suspending
 * will write out all the bins.
 **/
static void suspendAndResumePackerTest(void)
{
  allBinsFull     = false;
  packedItemCount = 0;
  setCompletionEnqueueHook(wrapIfHeadingToPacker);
  IORequest *request
    = launchIndexedWrite(0, DEFAULT_PACKER_BINS * 2, 1);
  waitForState(&allBinsFull);
  performSuccessfulPackerAction(VDO_ADMIN_STATE_SUSPENDING);
  awaitAndFreeSuccessfulRequest(UDS_FORGET(request));

  // Make sure all bins show all their block space free.
  struct packer *packer = vdo->packer;
  for (struct packer_bin *bin = vdo_get_packer_fullest_bin(packer);
       bin != NULL;
       bin = vdo_next_packer_bin(packer, bin)) {
    CU_ASSERT_EQUAL(bin->free_space, packer->bin_data_size);
  }

  performSuccessfulPackerAction(VDO_ADMIN_STATE_RESUMING);
}

/**
 * Check that the fullest bin has 2 empty slots, and all other bins are empty.
 *
 * Implements vdo_action
 **/
static void checkFullestBin(struct vdo_completion *completion)
{
  size_t expected = VDO_MAX_COMPRESSION_SLOTS - 2;

  for (struct packer_bin *bin = vdo_get_packer_fullest_bin(vdo->packer);
       bin != NULL;
       bin = vdo_next_packer_bin(vdo->packer, bin)) {
    CU_ASSERT_EQUAL(bin->slots_used, expected);
    expected = 0;
  }

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**********************************************************************/
static void removeVIOsTest(void)
{
  const size_t slots = VDO_MAX_COMPRESSION_SLOTS;
  block_count_t freeBlocks = getPhysicalBlocksFree();

  // add all but one fragment
  packedItemCount = 0;
  targetItemCount = slots - 1;
  packed = false;
  shouldQueue = false;
  setCompletionEnqueueHook(wrapIfLeavingCompressor);

  IORequest *requests[targetItemCount];
  for (block_size_t i = 0; i < targetItemCount; i++) {
    compressedSizes[i] = i + 1;
    requests[i] = launchIndexedWrite(i, 1, i);
  }

  waitForState(&packed);

  // Remove a fragment by issuing a write with the same data. Even though
  // dedupe is disabled, concurrent dedupe is not.
  shouldQueue = true;
  writeData(slots * 2, 4, 1, VDO_SUCCESS);
  awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[4]));

  expectedSlotsUsed = slots - 2;
  performSuccessfulActionOnThread(checkFullestBin,
                                  vdo->thread_config->packer_thread);

  // add two more to fill the bin
  packed = false;
  shouldQueue = false;
  targetItemCount++;
  compressedSizes[slots] = 1;
  requests[4] = launchIndexedWrite(slots, 1, slots);
  waitForState(&packed);

  shouldQueue = true;
  compressedSizes[slots + 1] = 1;
  writeData(slots + 1, slots + 1, 1, VDO_SUCCESS);

  // wait for output vios
  for (size_t i = 0; i < slots - 1; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
  }

  // We should have written exactly 2 blocks.
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), freeBlocks - 2);
}

/**********************************************************************/

static CU_TestInfo packerTests[] = {
  { "suspend empty packer test",      suspendEmptyPackerTest     },
  { "suspend and resume packer test", suspendAndResumePackerTest },
  { "bin boundary test",              binBoundaryTest            },
  { "best fit test",                  bestFitTest                },
  { "remove vios test",               removeVIOsTest             },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo packerSuite = {
  .name                     = "packer tests (Packer_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = packerTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &packerSuite;
}
