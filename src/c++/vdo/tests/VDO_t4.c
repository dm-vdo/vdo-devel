/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "uds.h"

#include "dedupe.h"
#include "slab-depot.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static block_count_t               dataBlocks;
static logical_block_number_t      finalWriteBlock;
static physical_block_number_t     pbn2;
static physical_block_number_t     pbn3;

/**
 * Test-specific initialization.
 **/
static void initializeVDOT4(void)
{
  TestParameters parameters = {
    .mappableBlocks      = 64,
    .dataFormatter       = fillWithOffsetPlusOne,
    .physicalThreadCount = 1,
  };
  initializeVDOTest(&parameters);
}

/**
 * Hook to record where LBNs 2 and 3 have written their data.
 *
 * Implements CompletionHook.
 **/
static bool recordMapping(struct vdo_completion *completion)
{
  if (!isDataWrite(completion)) {
    return true;
  }

  if (logicalIs(completion, 2)) {
    pbn2 = as_data_vio(completion)->new_mapped.pbn;
  } else if (logicalIs(completion, 3)) {
    pbn3 = as_data_vio(completion)->new_mapped.pbn;
  }

  return true;
}

/**
 * Fill the physical space and record the mappings for LBNs 2 and 3.
 **/
static void fillVDO(void)
{
  setCompletionEnqueueHook(recordMapping);
  dataBlocks = fillPhysicalSpace(0, 0);
  clearCompletionEnqueueHooks();
  finalWriteBlock = dataBlocks + 1;
}

/**
 * Implements BlockCondition.
 **/
static bool
shouldBlockVIO(struct vdo_completion *completion,
               void                  *context __attribute__((unused)))
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_VERIFY_DUPLICATION)) {
    return false;
  }

  struct data_vio *dataVIO = as_data_vio(completion);
  struct pbn_lock *duplicateLock = vdo_get_duplicate_lock(dataVIO);
  CU_ASSERT_PTR_NOT_NULL(duplicateLock);
  CU_ASSERT_FALSE(vdo_pbn_lock_has_provisional_reference(duplicateLock));
  CU_ASSERT_EQUAL(dataVIO->duplicate.pbn, pbn2);
  return true;
}

/**
 * Implements CompletionHook.
 **/
static bool releaseBlockedVIOAfterAllocation(struct vdo_completion *completion)
{
  if (!logicalIs(completion, finalWriteBlock)
      || !data_vio_has_allocation(as_data_vio(completion))) {
    return true;
  }

  clearCompletionEnqueueHooks();

  /*
   * We have our new write request which should have tried and failed to get a
   * write lock on pbn2. Confirm that this is the case and then let the request
   * for LBN 1 proceed.
   */
  struct vio      *blocked = getBlockedVIO();
  struct data_vio *dataVIO = vio_as_data_vio(blocked);
  bool hasReference
    = vdo_pbn_lock_has_provisional_reference(vdo_get_duplicate_lock(dataVIO));
  CU_ASSERT_TRUE(hasReference);
  CU_ASSERT_NOT_EQUAL(dataVIO->allocation.pbn,
                      as_data_vio(completion)->allocation.pbn);
  reallyEnqueueVIO(blocked);
  return true;
}

/**
 * Issue a write which will cause the blocked write to be released.
 *
 * @param blockedWrite    The blocked write which will be waited on and freed
 * @param expectedResult  The expected result of the blocked write
 **/
static void doFinalWrite(IORequest *blockedWrite, int expectedResult)
{
  setCompletionEnqueueHook(releaseBlockedVIOAfterAllocation);
  writeData(finalWriteBlock, finalWriteBlock, 1, VDO_SUCCESS);

  CU_ASSERT_EQUAL(expectedResult,
                  awaitAndFreeRequest(uds_forget(blockedWrite)));

  // Verify the write to finalWriteBlock (which we couldn't do before as the
  // verification would race with the completion of the request for LBN 1).
  verifyWrite(finalWriteBlock, finalWriteBlock, 1, 1, dataBlocks - 1);
}

/**********************************************************************/
static void verifyReferenceStatus(physical_block_number_t pbn,
                                  enum reference_status   expectedStatus)
{
  struct vdo_slab *slab = vdo_get_slab(vdo->depot, pbn);
  enum reference_status status;
  VDO_ASSERT_SUCCESS(getReferenceStatus(slab, pbn, &status));
  CU_ASSERT_EQUAL(expectedStatus, status);
}

/**********************************************************************/
static void testVerificationRaceWithTrim(void)
{
  fillVDO();

  // Write a duplicate of LBN 2 at LBN 1 and block the callback after the
  // read-verify.
  setBlockVIOCompletionEnqueueHook(shouldBlockVIO, true);
  IORequest *request = launchIndexedWrite(1, 1, 2);
  waitForBlockedVIO();

  // Trim LBNs 2 and 3.
  discardData(2, 2, VDO_SUCCESS);

  /*
   * Write new data which should attempt to get a write lock on the PBN which
   * was mapped to LBN 2 and then end up being written to the PBN which had
   * been mapped to LBN 3.
   */
  doFinalWrite(uds_forget(request), VDO_SUCCESS);

  // Verify the data from the initial write.
  verifyData(1, 2, 1);

  // Verify the reference counts of virtual PBNs 2 and 3.
  verifyReferenceStatus(pbn2, RS_SINGLE);
  verifyReferenceStatus(pbn3, RS_SINGLE);
}

/**
 * Implements CompletionHook.
 **/
static bool falsifyAdvice(struct vdo_completion *completion)
{
  if (!lastAsyncOperationIs(completion, VIO_ASYNC_OP_CHECK_FOR_DUPLICATION)) {
    return true;
  }

  removeCompletionEnqueueHook(falsifyAdvice);

  struct data_vio *dataVIO = as_data_vio(completion);
  CU_ASSERT_FALSE(dataVIO->is_duplicate);

  struct uds_request     *request  = &dataVIO->dedupe_context->request;
  struct uds_record_data *encoding = &request->old_metadata;
  size_t                  offset   = 0;
  encoding->data[offset++] = 2; // UDS_ADVICE_VERSION
  encoding->data[offset++] = VDO_MAPPING_STATE_UNCOMPRESSED;
  put_unaligned_le64(pbn2, &encoding->data[offset]);

  request->status = UDS_SUCCESS;
  request->found = true;
  setBlockVIOCompletionEnqueueHook(shouldBlockVIO, true);
  return true;
}

/**********************************************************************/
static void testDecrementAfterIncorrectSpeculativeIncrement(void)
{
  fillVDO();

  // Write new data and lie about it being a duplicate.
  setCompletionEnqueueHook(falsifyAdvice);
  IORequest *request
    = launchIndexedWrite(finalWriteBlock + 1, 1, finalWriteBlock + 1);
  waitForBlockedVIO();

  // Trim LBNs 2 and 3.
  discardData(2, 2, VDO_SUCCESS);

  /*
   * Write new data which should attempt to get a write lock on the PBN which
   * was mapped to LBN 2 and then end up being written to the PBN which had
   * been mapped to LBN 3.
   */
  doFinalWrite(uds_forget(request), VDO_NO_SPACE);
  writeAndVerifyData(finalWriteBlock + 2, finalWriteBlock + 2, 1, 0,
                     dataBlocks);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test verify vs. trim", testVerificationRaceWithTrim },
  { "test clearing of incorrect refcount increment",
    testDecrementAfterIncorrectSpeculativeIncrement },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name        = "vdo physical block locking tests (VDO_t4)",
  .initializerWithArguments = NULL,
  .initializer              = initializeVDOT4,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
