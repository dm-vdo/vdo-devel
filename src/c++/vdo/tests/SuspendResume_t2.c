/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "logger.h"

#include "ioRequest.h"
#include "vdoTestBase.h"

/**
 * Initialize with multiple threads.
 **/
static void initialize(void)
{
  unsigned int slabCount = 16;
  unsigned int slabSize = 512;
  const TestParameters parameters = {
    .logicalThreadCount  = 3,
    .physicalThreadCount = 2,
    .hashZoneThreadCount = 2,
    .slabCount = slabCount,
    .slabJournalBlocks = 8,
    .slabSize = slabSize,
    // Geometry block + super block + root count + slabs + recovery journal
    // + slab summary
    .physicalBlocks = 1 + 1 + 60 + (slabCount * slabSize) + 32 + 64,
  };
  initializeVDOTest(&parameters);
}

/**
 * Test suspend and resume of a VDO device, exercising journal paths
 * to make sure locks are cleared.
 *
 * @param save  If true, save all dirty metadata when suspending
 **/
static void suspendResumeTest(bool save)
{
  int j;
  for (j = 0; j < 10; j++) {
    {
      int i;
      for (i = 0; i < 100; i++) {
	writeData((i * 16) % 3000, (i + 1) * 12, 16, VDO_SUCCESS);
      }
    }

    // Write some data
    writeData(0, 0, 16, VDO_SUCCESS);

    // Suspend a dirty VDO
    performSuccessfulSuspendAndResume(save);

    // Verify the data
    verifyData(0, 0, 16);
  }

  {
    int i;
    for (i = 0; i < 1000; i++) {
      writeData((i * 16) % 3000, (i + 1) * 12, 16, VDO_SUCCESS);
    }
  }

  // Write some more
  writeData(0, 0, 16, VDO_SUCCESS);
  writeData(16, 16, 16, VDO_SUCCESS);

  // Suspend again
  performSuccessfulSuspendAndResume(save);

  verifyData(0, 0, 16);
  verifyData(16, 16, 16);

  // Shutdown
  restartVDO(false);
  verifyData(0, 0, 16);
  verifyData(16, 16, 16);
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
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "suspend and resume (SuspendResume_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
