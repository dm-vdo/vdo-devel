/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "vio.h"

#include "physicalLayer.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  LOGICAL_BLOCKS          = 800, // Less than one block map block.
  MAPPABLE_BLOCKS         = 64,
  RECOVERY_JOURNAL_BLOCKS = 8,
};

static const TestParameters TEST_PARAMETERS = {
  .mappableBlocks = MAPPABLE_BLOCKS,
  .logicalBlocks  = LOGICAL_BLOCKS,
  .journalBlocks  = RECOVERY_JOURNAL_BLOCKS,
};

// Which 512-byte chunks to corrupt.
static uint8_t         corruption;
static bool            hookFired;
static bool            signalOnTear;

/**
 * Set up the test.
 **/
static void initializeTornWritesT1(void)
{
  initializeVDOTest(&TEST_PARAMETERS);
  hookFired = false;
}

/**
 * Cause the VIO to have a torn write, by combining its current data with
 * 1 to 7 512-byte blocks of the current on-disk value of the block.
 *
 * @param vio             The VIO which is getting a torn write
 * @param corruptRegions  A set of 8 flags indicating which 512-byte chunks to
 *                        replace with the current on-disk values.
 **/
static void tearVIO(struct vio *vio, uint8_t corruptRegions)
{
  char *currentDiskData;
  VDO_ASSERT_SUCCESS(vdo_allocate(VDO_BLOCK_SIZE, char, __func__,
                                  &currentDiskData));
  VDO_ASSERT_SUCCESS(layer->reader(layer,
                                   pbnFromVIO(vio),
                                   1,
                                   currentDiskData));
  for (off_t chunkOffset = 0; chunkOffset < 8; chunkOffset++) {
    if ((1 << chunkOffset) & corruptRegions) {
      // This region should be corrupted in the VIO with the on-disk data.
      memcpy(&vio->data[512 * chunkOffset],
             &currentDiskData[512 * chunkOffset], 512);
    }
  }
  vdo_free(currentDiskData);
}

/**
 * Check whether a VIO is doing a block map write.
 *
 * @param vio  The VIO to check
 *
 * @return <code>true</code> if the VIO is doing a block map write
 **/
static bool isBlockMapWrite(struct vio *vio)
{
  return ((vio->type == VIO_TYPE_BLOCK_MAP)
          && isMetadataWrite(&vio->completion));
}

/**
 * Prevent the next block map write from actually happening.
 *
 * Implements BIOSubmitHook.
 **/
static bool skipNextBlockMapWrite(struct bio *bio) {
  if (!isBlockMapWrite(bio->bi_private)) {
    return true;
  }

  clearBIOSubmitHook();
  signalState(&hookFired);
  bio->bi_end_io(bio);
  return false;
}

/**
 * Catch the first block map write and set up to prevent the second
 * block map write (due to torn write protection) from happening.
 *
 * Implements BIOSubmitHook.
 **/
static bool catchFirstWrite(struct bio *bio)
{
  if (isBlockMapWrite(bio->bi_private)) {
    setBIOSubmitHook(skipNextBlockMapWrite);
  }

  return true;
}

/**
 * Optionally write data to LBN 0, write zero blocks to LBN 1 until the block
 * map tries to write, then crash the VDO, restart it, and verify that the data
 * and affected block map page are correct.
 **/
static void tearBlockMapPage(bool writeToLBN0)
{
  // Write a block of data to LBN 0.
  if (writeToLBN0) {
    writeData(0, 1, 1, VDO_SUCCESS);
  }

  // Write a bunch of zero blocks to LBN 1, which should generate
  // lots of journal entries, until the block map tries to write.
  while (!checkState(&hookFired)) {
    zeroData(1, 1, VDO_SUCCESS);
  }

  crashVDO();
  startVDO(VDO_DIRTY);

  // Verify that the data is readable, and the rest of the block map page
  // is properly zeros.
  verifyData(0, 1, 1);
  verifyZeros(1, LOGICAL_BLOCKS - 1);
}

/**
 * Test the effect of a lost write on the block map. The page should be
 * treated as uninitialized.
 **/
static void testBlockMapLostWrite(void)
{
  setBIOSubmitHook(catchFirstWrite);
  tearBlockMapPage(true);
}

/**
 * Implements BIOSubmitHook
 **/
static bool tearMetadataWrite(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if (!isBlockMapWrite(vio)) {
    return true;
  }

  tearVIO(vio, corruption);
  clearBIOSubmitHook();
  if (signalOnTear) {
    signalState(&hookFired);
  }

  return true;
}

/**
 * Signal that the callback after a block map write has finished.
 *
 * <p>Implements VDOAction.
 **/
static void blockMapWriteFinished(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  signalState(&hookFired);
}

/**
 * If this is the callback after a block map write, prepare to crash the VDO.
 *
 * <p>Implements CompletionHook.
 **/
static bool prepareToCrashOnBlockMapWrite(struct vdo_completion *completion)
{
  if (is_vio(completion) && isBlockMapWrite(as_vio(completion))) {
    prepareToCrashRAMLayer(getSynchronousLayer());
    wrapCompletionCallback(completion, blockMapWriteFinished);
    clearCompletionEnqueueHooks();
  }

  return true;
}

/**
 * Test the effect of a torn write on the first write of a previously
 * uninitialized block map page.
 **/
static void testBlockMapInitialTornWrite(void)
{
  signalOnTear = false;
  setBIOSubmitHook(tearMetadataWrite);
  setCompletionEnqueueHook(prepareToCrashOnBlockMapWrite);
  corruption = 0xf0;
  tearBlockMapPage(true);
  clearCompletionEnqueueHooks();
}

/**
 * Catch the first block map write and set up to tear the second
 * block map write (due to torn write protection).
 *
 * Implements CompletionHook.
 **/
static bool tearSecondWrite(struct vdo_completion *completion)
{
  if (!onBIOThread() || !isBlockMapWrite(as_vio(completion))) {
    return true;
  }

  clearCompletionEnqueueHooks();
  corruption = 0xf0;
  signalOnTear = true;
  setBIOSubmitHook(tearMetadataWrite);
  return true;
}

/**
 * Test the effect of a torn write on the rewrite of a previously uninitialized
 * block map page.
 **/
static void testBlockMapInitialTornRewrite(void)
{
  // Catch the first block map write which will set up to tear the second
  setCompletionEnqueueHook(tearSecondWrite);
  tearBlockMapPage(true);
}

/**
 * Test that a torn write of a block map write after the relevant block has
 * been written once.
 **/
static void testBlockMapSubsequentTornWrite(void)
{
  writeData(0, 1, 1, VDO_SUCCESS);
  restartVDO(false);

  /**
   * Now we are guaranteed the page containing LBN 0 has been written to
   * disk completely at least once. Tear its next write, corrupting all but
   * the 0th chunk.
   **/
  corruption   = (~(1 << 0));
  signalOnTear = true;
  setBIOSubmitHook(tearMetadataWrite);
  tearBlockMapPage(false);
}

/**********************************************************************/
static CU_TestInfo tornWriteTests[] = {
  { "test block map write loss",           testBlockMapLostWrite           },
  { "test block map initial torn write",   testBlockMapInitialTornWrite    },
  { "test block map initial torn rewrite", testBlockMapInitialTornRewrite  },
  { "test block map subseq. torn write",   testBlockMapSubsequentTornWrite },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo tornWriteSuite = {
  .name                     = "Torn block map writes (TornWrites_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeTornWritesT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tornWriteTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &tornWriteSuite;
}
