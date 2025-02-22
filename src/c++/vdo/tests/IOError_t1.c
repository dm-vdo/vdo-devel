/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/bio.h>

#include "asyncLayer.h"
#include "ioRequest.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static unsigned int errorOperation;

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 16,
   };
  initializeVDOTest(&parameters);
}

/**
 * Mark I/O of the given type as a failure.
 *
 * Implements BIOSubmitHook.
 **/
static bool injectError(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((bio_op(bio) != errorOperation) || (vio->type != VIO_TYPE_DATA)) {
    return true;
  }

  bio->bi_status = BLK_STS_VDO_INJECTED;
  bio->bi_end_io(bio);
  clearBIOSubmitHook();
  return false;
}

/**********************************************************************/
static void testDataReadError(void)
{
  char buffer[VDO_BLOCK_SIZE];

  writeData(0, 1, 1, VDO_SUCCESS);
  errorOperation = REQ_OP_READ;
  setBIOSubmitHook(injectError);
  CU_ASSERT_EQUAL(BLK_STS_VDO_INJECTED, performRead(0, 1, buffer));
  // Confirm that we're not read-only
  setStartStopExpectation(VDO_SUCCESS);
  restartVDO(false);
}

/**********************************************************************/
static void testReadVerifyError(void)
{
  writeData(0, 1, 1, VDO_SUCCESS);
  errorOperation = REQ_OP_READ;
  setBIOSubmitHook(injectError);
  writeAndVerifyData(1,
                     1,
                     1,
                     getPhysicalBlocksFree() - 1,
                     vdo_get_physical_blocks_allocated(vdo) + 1);
  // Confirm that we're not read-only
  setStartStopExpectation(VDO_SUCCESS);
  restartVDO(false);
}

/**********************************************************************/
static void testDataWriteError(void)
{
  errorOperation = REQ_OP_WRITE;
  setBIOSubmitHook(injectError);
  writeData(0, 1, 1, BLK_STS_VDO_INJECTED);
  // Confirm that we're read-only
  setStartStopExpectation(VDO_READ_ONLY);
  stopVDO();
  startVDO(VDO_READ_ONLY_MODE);
}

/**********************************************************************/
static void testCompressedWriteError(void)
{
  populateBlockMapTree();
  modifyCompressDedupe(true, true);
  errorOperation = REQ_OP_WRITE;
  setBIOSubmitHook(injectError);
  writeAndVerifyData(0,
                     1,
                     VDO_MAX_COMPRESSION_SLOTS,
                     getPhysicalBlocksFree() - VDO_MAX_COMPRESSION_SLOTS,
                     (vdo_get_physical_blocks_allocated(vdo)
                      + VDO_MAX_COMPRESSION_SLOTS));
  // Confirm that we're not read-only
  setStartStopExpectation(VDO_SUCCESS);
  restartVDO(false);
}


/**********************************************************************/
static CU_TestInfo ioErrorTests[] = {
  { "data read I/O error",              testDataReadError         },
  { "read verify I/O error",            testReadVerifyError       },
  { "data write I/O error",             testDataWriteError        },
  { "compressed data write I/O error",  testCompressedWriteError  },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "Data I/O tests (IOError_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = ioErrorTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
