/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/bio.h>

#include "block-map.h"
#include "block-map-page.h"
#include "read-only-notifier.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "ioRequest.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

#include "logger.h"

enum {
  INJECTED_ERROR = -1,
};

static enum vio_type errorType;
static unsigned int errorOperation;

/**
 * Test-specific initialization.
 **/
static void initializeReadOnlyModeT1(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 16,
    .journalBlocks  = 4,
  };
  initializeVDOTest(&parameters);
}

/**
 * Verify read-only mode: writes fails, but reads succeed.
 **/
static void verifyReadOnlyMode(void)
{
  assertVDOState(VDO_READ_ONLY_MODE);
  writeData(11, 0, 2, VDO_READ_ONLY);
  verifyData(1, 0, 10);
  setStartStopExpectation(VDO_READ_ONLY);
}

/**
 * Verify that read-only mode persists across a restart.
 **/
static void verifyReadOnlyModePersistsOnce(void)
{
  verifyReadOnlyMode();
  stopVDO();
  startVDO(VDO_READ_ONLY_MODE);
}

/**
 * Verify read-only mode persists across restarts.
 **/
static void verifyReadOnlyModePersistence(void)
{
  verifyReadOnlyModePersistsOnce();
  verifyReadOnlyModePersistsOnce();
  CU_ASSERT_EQUAL(VDO_READ_ONLY, suspendVDO(false));
  CU_ASSERT_EQUAL(VDO_READ_ONLY,
                  resumeVDO(vdo->device_config->owning_target));
  verifyReadOnlyModePersistsOnce();
}

/**
 * Mark I/O of the given type as a failure.
 *
 * Implements BIOSubmitHook.
 **/
static bool injectError(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((bio_op(bio) != errorOperation) || (vio->type != errorType)) {
    return true;
  }

  bio->bi_status = INJECTED_ERROR;
  bio->bi_end_io(bio);
  return false;
}

/**********************************************************************/
static void testWithIOError(enum vio_type type,
                            unsigned int operation,
                            int expectedResult)
{
  writeData(1, 0, 10, VDO_SUCCESS);
  restartVDO(false);
  errorType = type;
  errorOperation = operation;
  setBIOSubmitHook(injectError);
  writeData(1, 0, 1, expectedResult);
  assertVDOState(VDO_READ_ONLY_MODE);
  clearBIOSubmitHook();
  verifyReadOnlyModePersistence();
}

/**
 * Test VDO read only mode caused by a journal write succeeding followed by
 * the block map read failing.
 **/
static void testBlockMapWriteFailure(void)
{
  testWithIOError(VIO_TYPE_BLOCK_MAP, REQ_OP_READ, INJECTED_ERROR);
}

/**
 * Test VDO read only mode caused by a recovery journal block write error.
 **/
static void testRecoveryJournalWriteFailure(void)
{
  testWithIOError(VIO_TYPE_RECOVERY_JOURNAL, REQ_OP_WRITE, VDO_READ_ONLY);
}

/**
 * Test VDO read only mode caused by a superblock write failing on load.
 **/
static void testSuperBlockWriteFailure(void)
{
  writeData(1, 0, 10, VDO_SUCCESS);
  stopVDO();
  errorType = VIO_TYPE_SUPER_BLOCK;
  errorOperation = REQ_OP_WRITE;
  setBIOSubmitHook(injectError);
  startReadOnlyVDO(VDO_CLEAN);
  assertVDOState(VDO_READ_ONLY_MODE);
  clearBIOSubmitHook();
  verifyReadOnlyMode();
}

/**********************************************************************/
static void notEnteringAction(struct vdo_completion *completion)
{
  struct read_only_notifier *notifier = vdo->read_only_notifier;
  vdo_wait_until_not_entering_read_only_mode(notifier, completion);
}

/**********************************************************************/
static void allowEnteringAction(struct vdo_completion *completion)
{
  vdo_allow_read_only_mode_entry(vdo->read_only_notifier, completion);
}

/**
 * Test re-enabling of read-only mode entry.
 **/
static void testAllowReadOnlyModeEntry(void)
{
  writeData(1, 0, 10, VDO_SUCCESS);
  restartVDO(false);
  performSuccessfulAction(notEnteringAction);
  performSuccessfulAction(allowEnteringAction);
  forceVDOReadOnlyMode();
  verifyReadOnlyModePersistence();
}

/**********************************************************************/
static void enterAction(struct vdo_completion *completion)
{
  vdo_enter_read_only_mode(vdo->read_only_notifier, VDO_NOT_IMPLEMENTED);
  assertVDOState(VDO_DIRTY);
  vdo_complete_completion(completion);
}

/**
 * Test delayed read-only mode entry.
 **/
static void testDelayedReadOnlyModeEntry(void)
{
  writeData(1, 0, 10, VDO_SUCCESS);
  restartVDO(false);
  performSuccessfulAction(notEnteringAction);
  performSuccessfulAction(enterAction);
  performSuccessfulAction(allowEnteringAction);
  verifyReadOnlyModePersistence();
}

/**
 * Test entering read-only mode from a non-VDO thread.
 **/
static void testReadOnlyEntryFromNonVDOThread(void)
{
  writeData(1, 0, 10, VDO_SUCCESS);
  restartVDO(false);
  vdo_enter_read_only_mode(vdo->read_only_notifier, VDO_NOT_IMPLEMENTED);
  performSuccessfulAction(notEnteringAction);
  verifyReadOnlyModePersistence();
}


/**********************************************************************/
static CU_TestInfo readOnlyModeTests[] = {
  { "recovery journal write failure",      testRecoveryJournalWriteFailure   },
  { "post-journaling block map failure",   testBlockMapWriteFailure          },
  { "loadtime super block write failure",  testSuperBlockWriteFailure        },
  { "re-enabling of read-only mode entry", testAllowReadOnlyModeEntry        },
  { "delayed read-only mode entry",        testDelayedReadOnlyModeEntry      },
  { "enter read-only from non-vdo thread", testReadOnlyEntryFromNonVDOThread },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "Read only mode tests (ReadOnlyMode_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeReadOnlyModeT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = readOnlyModeTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
