/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "flush.h"
#include "vdo.h"
#include "vdo-resize-logical.h"
#include "vdo-resume.h"

#include "asyncLayer.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "testBIO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  LAYER_ERROR = -1,
};

static const block_count_t NEW_LOGICAL_SIZE = 100000000;

static bool success;
static bool empty;
static bool save;

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    // Make sure the recovery journal is long enough that tree pages are not
    // written immediately.
    .journalBlocks = 16,
  };
  initializeVDOTest(&parameters);
  success = true;
  empty   = true;
  save    = false;
}

/**
 * Fail a super block write.
 *
 * Implements BIOSubmitHook.
 **/
static bool failSuperBlockWrite(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((vio == NULL)
      || (vio->type != VIO_TYPE_SUPER_BLOCK)
      || (bio_op(vio->bio) != REQ_OP_WRITE)) {
    return true;
  }

  // Take out this hook.
  clearBIOSubmitHook();

  // Set a bad error code to force a failed write.
  bio->bi_status = LAYER_ERROR;

  // Don't do the write.
  bio->bi_end_io(bio);
  return false;
}

/**********************************************************************/
static void testGrowLogical(void)
{
  block_count_t startingLogicalSize = getTestConfig().config.logical_blocks;

  // Writing to an out-of-bounds location doesn't work.
  writeData(startingLogicalSize, 1, 1, VDO_OUT_OF_RANGE);

  if (!empty) {
    // Write some data
    writeData(0, 1, 1, VDO_SUCCESS);
  }

  block_count_t expectedSize;
  int newRangeResult;
  if (success) {
    expectedSize   = NEW_LOGICAL_SIZE;
    newRangeResult = VDO_SUCCESS;
  } else {
    setBIOSubmitHook(failSuperBlockWrite);
    setStartStopExpectation(VDO_READ_ONLY);
    expectedSize   = startingLogicalSize;
    newRangeResult = VDO_OUT_OF_RANGE;
  }

  // Attempt to grow
  CU_ASSERT_EQUAL((success ? VDO_SUCCESS : LAYER_ERROR),
                  growVDOLogical(NEW_LOGICAL_SIZE, save));
  CU_ASSERT_EQUAL(expectedSize, getTestConfig().config.logical_blocks);

  logical_block_number_t newRangeLBN = NEW_LOGICAL_SIZE - 1;

  // Try reading from the new range.
  if (success) {
    verifyZeros(newRangeLBN, 1);
  } else {
    // The VDO failed to resume, so resume it again.
    VDO_ASSERT_SUCCESS(vdo_preresume_internal(vdo,
                                              vdo->device_config,
                                              "test device"));
  }

  // Try writing to the new range.
  writeData(newRangeLBN, 1, 1, newRangeResult);

  // Now destroy the running VDO without saving.
  crashVDO();

  // The read-only state can not persist.
  setStartStopExpectation(VDO_SUCCESS);
  startVDO(VDO_DIRTY);
  CU_ASSERT_EQUAL(expectedSize, getTestConfig().config.logical_blocks);

  if (!empty) {
    // Verify the data written before the growth.
    verifyData(0, 1, 1);
  }

  if (success) {
    // Can still read and write the new logical range.
    verifyData(expectedSize - 1, 1, 1);
  } else {
    // Get the VDO out of read-only mode
    rebuildReadOnlyVDO();
  }

  writeData(0, 2, 1, VDO_SUCCESS);
  writeData(newRangeLBN, 2, 1, newRangeResult);

  // Restart cleanly
  restartVDO(false);
  CU_ASSERT_EQUAL(expectedSize, getTestConfig().config.logical_blocks);

  verifyData(0, 2, 1);
  if (success) {
    verifyData(newRangeLBN, 2, 1);
  }

  writeData(0, 3, 1, VDO_SUCCESS);
  writeData(newRangeLBN, 3, 1, newRangeResult);

  verifyData(0, 3, 1);
  if (success) {
    verifyData(newRangeLBN, 3, 1);
  }
}

/**********************************************************************/
static void testGrowLogicalWithSave(void)
{
  save = true;
  empty = false;
  testGrowLogical();
}

/**********************************************************************/
static void testGrowLogicalNotEmpty(void)
{
  empty = false;
  testGrowLogical();
}

/**********************************************************************/
static void testGrowLogicalFailure(void)
{
  success = false;
  testGrowLogical();
}

/**********************************************************************/
static void testGrowLogicalFailureNotEmpty(void)
{
  success = false;
  empty = false;
  testGrowLogical();
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "grow logical succeeds, empty VDO",               testGrowLogical                },
  { "grow logical succeeds, non-empty VDO",           testGrowLogicalNotEmpty        },
  { "grow logical fails, empty VDO",                  testGrowLogicalFailure         },
  { "grow logical fails, non-empty VDO",              testGrowLogicalFailureNotEmpty },
  { "grow logical with save succeeds, non-empty VDO", testGrowLogicalWithSave        },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "GrowLogical_t1",
  .initializer = initialize,
  .cleaner     = tearDownVDOTest,
  .tests       = tests
};

/**********************************************************************/
CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
