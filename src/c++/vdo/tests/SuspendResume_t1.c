/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "logger.h"

#include "vdo-resume.h"
#include "vdo-suspend.h"

#include "ioRequest.h"
#include "vdoTestBase.h"

/**
 * Initialize with multiple threads.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .logicalThreadCount  = 3,
    .physicalThreadCount = 2,
    .hashZoneThreadCount = 2,
  };
  initializeVDOTest(&parameters);
}

/**
 * Test suspend and resume of a VDO device.
 *
 * @param save  If true, save all dirty metadata when suspending
 **/
static void suspendResumeTest(bool save)
{
  // Suspend new VDO
  performSuccessfulSuspendAndResume(save);

  // Write some data
  writeData(0, 0, 16, VDO_SUCCESS);

  // Suspend a dirty VDO
  performSuccessfulSuspendAndResume(save);

  // Verify the data
  verifyData(0, 0, 16);

  // Write some more
  writeData(16, 16, 16, VDO_SUCCESS);

  // Suspend again
  performSuccessfulSuspendAndResume(save);

  verifyData(0, 0, 32);

  // Shutdown
  restartVDO(false);
  verifyData(0, 0, 32);
}

/**
 * Test suspend without save.
 **/
static void testSuspend(void)
{
  suspendResumeTest(false);
}

/**
 * Test suspend with save.
 **/
static void testSave(void)
{
  suspendResumeTest(true);
}

static CU_TestInfo vdoTests[] = {
  { "suspend and resume without saving", testSuspend },
  { "suspend and resume with saving",    testSave    },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "suspend and resume (SuspendResume_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
