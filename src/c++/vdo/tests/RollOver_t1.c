/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "packerUtils.h"
#include "slab-depot.h"
#include "vio.h"

#include "asyncLayer.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static enum async_operation_number  operationToLatch;
static block_count_t                latchedCount;
static block_count_t                viosToLatch;
static struct vio                  *latchedVIOs[259];

/**
 * An action wrapper to mark the first slab as unrecovered.
 **/
static void markOpenSlabUnrecovered(struct vdo_completion *completion)
{
  struct block_allocator *allocator   = &vdo->depot->allocators[0];
  struct vdo_slab        *currentSlab = allocator->open_slab;
  currentSlab->status = VDO_SLAB_REQUIRES_SCRUBBING;

  // Remove slab from list of non-full slabs.
  vdo_priority_table_remove(allocator->prioritized_slabs,
                            &currentSlab->allocq_entry);
  allocator->open_slab = NULL;

  // Add slab to the unrecovered slab list.
  vdo_register_slab_for_scrubbing(currentSlab, false);
  vdo_finish_completion(completion);
}

/**
 * Test that multiple deduplications roll over onto another block.
 **/
static void testRollOver(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 512,
    .journalBlocks  = 64,
    .logicalBlocks  = 384,
    .slabCount      = 2,
    .dataFormatter  = fillWithFortySeven,
  };
  initializeVDOTest(&parameters);
  block_count_t blocksFree = populateBlockMapTree();

  // Refer to the first block the maximum number of times.
  writeAndVerifyData(  0, 0, 64, blocksFree - 1, 1);
  writeAndVerifyData( 64, 0, 64, blocksFree - 1, 1);
  writeAndVerifyData(128, 0, 64, blocksFree - 1, 1);
  writeAndVerifyData(192, 0, 62, blocksFree - 1, 1);

  // Force roll-over to a second block, on the same slab.
  writeAndVerifyData(254, 0, 64, blocksFree - 2, 2);

  /*
   * XXX: The change to require provisional references when acquiring read
   * locks prevents us from getting any dedupe at all once the advice points
   * at an unrecovered slab. This will be fixed as VDOSTORY-190 progresses,
   * but it is no longer clear that this hack will be a viable way to test
   * this.
   */
  if (false) {
    performSuccessfulAction(markOpenSlabUnrecovered);
    // Force roll-over to a third block.
    writeAndVerifyData(318, 0, 64, blocksFree - 3, 3);
  }
}

/**
 * Mimic the perl Direct04 test.
 **/
static void testDirect04(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 1024,
    .journalBlocks  = 1024,
    .logicalBlocks  = 1024,
  };
  initializeVDOTest(&parameters);

  block_count_t blocksFree = populateBlockMapTree();
  IORequest *requests[1024];
  for (logical_block_number_t i = 0; i < 1024; i++) {
    requests[i] = launchIndexedWrite(i, 1, 1 + (i % 2));
  }

  // Wait for the VIOs to come back from writing.
  for (logical_block_number_t i = 0; i < 1024; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
  }

  verifyWrite(0, 1, 2, blocksFree - 6, 6);
}

/**
 * Implements LockedMethod.
 **/
static bool latchVIO(void *context)
{
  latchedVIOs[latchedCount++] = context;
  if (latchedCount == viosToLatch) {
    clearCompletionEnqueueHooks();
    return true;
  }

  return false;
}

/**
 * This CompletionEnqueueHook latches VIOs whose last operation matches what
 * was configured in operationToLatch.
 **/
static bool latchAfterAdvice(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, operationToLatch)) {
    return true;
  }

  runLocked(latchVIO, as_vio(completion));
  return false;
}

/**
 * Set up latching for the next N vios whose last async operation is a given
 * value.
 *
 * @param howMany    How many VIOs to latch
 * @param latchAfter The async operation to latch the VIOs after
 **/
static void latchVIOsAfter(block_count_t               howMany,
                           enum async_operation_number latchAfter)
{
  viosToLatch      += howMany;
  operationToLatch  = latchAfter;
  setCompletionEnqueueHook(latchAfterAdvice);
}

/**
 * Implements WaitCondition
 **/
static bool checkLatchCount(void *context __attribute__((unused)))
{
  return (latchedCount == viosToLatch);
}

/**
 * Release a latched VIO.
 *
 * @param index  The index of the latched VIO
 **/
static void releaseLatchedVIO(size_t index)
{
  reallyEnqueueVIO(latchedVIOs[index]);
  latchedVIOs[index] = NULL;
}

/**
 * Test that multiple deduplications roll over onto another block.
 **/
static void testConcurrentRollOver(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 512,
    .journalBlocks  = 64,
    .logicalBlocks  = 254 + 254 + 2,
    .slabCount      = 2,
  };
  initializeVDOTest(&parameters);

  latchedCount                    = 0;
  viosToLatch                     = 0;
  logical_block_number_t lbnsUsed = 0;

  /*
   * We attempt to construct this situation:
   *
   * LBNs 0 through 253 point to PBN 1.
   * LBN 254 will get advice for PBN1, and then roll over PBN1 and write PBN2.
   * LBN 255 through 507 will get advice for PBN1, then get advice for PBN2
   *     and deduplicate against PBN2.
   * LBN 508 will get advice for PBN1, then get advice for PBN2, then roll
   *     over onto PBN3.
   * LBN 509 will get advice for PBN1, then get advice for PBN2, then get
   *     advice for PBN3.
   */

  // Write LBNs 0 through 253, which should all dedupe on PBN1.
  IORequest *requests[254];
  for (int i = 0; i < 254; i++) {
    requests[i] = launchIndexedWrite(lbnsUsed++, 1, 1);
  }

  for (int i = 0; i < 254; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
  }

  CU_ASSERT_EQUAL(1, vdo_get_physical_blocks_allocated(vdo));

  // Launch LBNs 254 through 507, and wait for them all to get advice for PBN1.
  // Make sure LBN 254 is latched first, for ordering purposes.
  IORequest *pbn2Requests[254];
  latchVIOsAfter(1, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION);
  pbn2Requests[0] = launchIndexedWrite(lbnsUsed++, 1, 1);
  waitForCondition(checkLatchCount, NULL);

  latchVIOsAfter(253, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION);
  for (int i = 1; i < 254; i++) {
    pbn2Requests[i] = launchIndexedWrite(lbnsUsed++, 1, 1);
  }

  waitForCondition(checkLatchCount, NULL);

  // Launch LBNs 508 and 509, and wait for their first advice (PBN1).
  IORequest *pbn3Requests[2];
  for (int i = 0; i < 2; i++) {
    latchVIOsAfter(1, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION);
    pbn3Requests[i] = launchIndexedWrite(lbnsUsed++, 1, 1);
    waitForCondition(checkLatchCount, NULL);
  }

  // Release LBN 254, which will roll over PBN1, and wait for it to complete.
  releaseLatchedVIO(0);
  awaitAndFreeSuccessfulRequest(UDS_FORGET(pbn2Requests[0]));

  // Release LBNs 255-507, which should dedupe against PBN2.
  for (int i = 1; i < 254; i++) {
    releaseLatchedVIO(i);
  }

  for (int i = 1; i < 254; i++) {
    awaitAndFreeSuccessfulRequest(UDS_FORGET(pbn2Requests[i]));
  }

  // Two blocks should have 254 references each, and two more should be
  // provisionally allocated.
  CU_ASSERT_EQUAL(4, vdo_get_physical_blocks_allocated(vdo));

  // Release LBN 508, and catch it on the way back from getting new advice
  // for PBN2 from the UDS index.
  // XXX this async op isn't used anymore
  // latchVIOsAfter(1, CHECK_FOR_DEDUPE_FOR_ROLLOVER);
  releaseLatchedVIO(254);
  waitForCondition(checkLatchCount, NULL); // latched at 256

  // Release LBN 508 again, and catch it on the way back from getting the
  // same advice for PBN2 from UDS.
  // XXX this async op isn't used anymore
  // latchVIOsAfter(1, CHECK_FOR_DEDUPE_FOR_ROLLOVER);
  releaseLatchedVIO(256);
  waitForCondition(checkLatchCount, NULL); // latched at 257

  // Release LBN 509, and catch it on the way back from getting new advice
  // for PBN2 from UDS.
  // XXX this async op isn't used anymore
  // latchVIOsAfter(1, CHECK_FOR_DEDUPE_FOR_ROLLOVER);
  releaseLatchedVIO(255);
  waitForCondition(checkLatchCount, NULL); // latched at 258

  // Release LBN 508, which will now finish rolling over and update UDS
  // with PBN3.
  releaseLatchedVIO(257);
  awaitAndFreeSuccessfulRequest(UDS_FORGET(pbn3Requests[0]));

  // Two blocks have 254 references, one has one reference, and one is
  // provisionally allocated still.
  CU_ASSERT_EQUAL(4, vdo_get_physical_blocks_allocated(vdo));

  // Release LBN 509, and make sure it correctly dedupes against PBN3.
  releaseLatchedVIO(258);
  awaitAndFreeSuccessfulRequest(UDS_FORGET(pbn3Requests[1]));

  // Exactly three blocks should be used now.
  CU_ASSERT_EQUAL(3, vdo_get_physical_blocks_allocated(vdo));
}

/**
 * Test that multiple compressed deduplications roll over onto another block.
 **/
static void testCompressRollOver(void)
{
  const TestParameters parameters = {
    .mappableBlocks    = 128,
    .journalBlocks     = 64,
    .logicalBlocks     = 384,
    .enableCompression = true,
  };
  initializeVDOTest(&parameters);
  block_count_t blocksFree = populateBlockMapTree();

  // Write a few compressible blocks. The packer notification cannot be active
  // when the VIOs are freed.
  unsigned int FRAGMENT_COUNT = VDO_MAX_COMPRESSION_SLOTS;
  IORequest *requests[VDO_MAX_COMPRESSION_SLOTS];
  setupPackerNotification();
  logical_block_number_t lbn;
  for (lbn = 0; lbn < FRAGMENT_COUNT - 1; lbn++) {
    requests[lbn] = launchIndexedWrite(lbn, 1, lbn + 1);
    waitForDataVIOToReachPacker();
  }

  tearDownPackerNotification();
  requests[lbn] = launchIndexedWrite(lbn, 1, FRAGMENT_COUNT + 1);
  lbn++;

  for (unsigned int i = 0; i < FRAGMENT_COUNT; i++) {
    // Wait for the VIOs to come back from the packer.
    awaitAndFreeSuccessfulRequest(UDS_FORGET(requests[i]));
  }

  // We now have 14 fragments in a compressed block.  Give it 230
  // more references:
  for (unsigned int i = 1; i <= 23; i++) {
    writeAndVerifyData(lbn, 1, 10, blocksFree - 1, 1);
    lbn += 10;
  }

  // There is room for ten more references.
  writeAndVerifyData(lbn, 1, 10, blocksFree - 1, 1);
  lbn += 10;

  // Force a roll-over (into the packer).
  setupPackerNotification();
  IORequest *rollOverRequest = launchIndexedWrite(lbn, 1, 1);
  waitForDataVIOToReachPacker();
  tearDownPackerNotification();
  requestFlushPacker();

  // Wait for the VIO to come back from the packer.
  awaitAndFreeSuccessfulRequest(UDS_FORGET(rollOverRequest));

  // The last write both completed and used just one more block.
  verifyWrite(lbn, 1, 1, blocksFree - 2, 2);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "roll-over of deduplicated block", testRollOver           },
  { "mimic Direct04",                  testDirect04           },
  { "roll-over of compressed block",   testCompressRollOver   },
  CU_TEST_INFO_NULL,
  // XXX VDOSTORY-190 changes the dedupe path, which breaks these, and will
  // continue to change it, so they're disabled until things stabilize.
  { "concurrent attempted roll-over",  testConcurrentRollOver },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Reference Count Roll-Over tests (RollOver_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
