/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**
 * Verify that block map tree changes can be made after restarts.
 **/
static void testBlockMapTreeModificationAfterRestart(void)
{
  logical_block_number_t lbn = 1;
  writeData(lbn, lbn, 1, VDO_SUCCESS);
  restartVDO(false);
  verifyData(1, 1, lbn - 1);

  lbn++;
  writeData(lbn, lbn, 1, VDO_SUCCESS);
  crashVDO();
  startVDO(VDO_DIRTY);
  verifyData(1, 1, lbn - 1);

  lbn++;
  writeData(lbn, lbn, 1, VDO_SUCCESS);
  rebuildReadOnlyVDO();
  verifyData(1, 1, lbn - 1);

  lbn++;
  writeData(lbn, lbn, 1, VDO_SUCCESS);
  restartVDO(false);
  verifyData(1, 1, lbn - 1);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test block map tree modification after restart (VDO-2377, VDO-3304)",
    testBlockMapTreeModificationAfterRestart },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "verify modifications of tree pages across restarts (BlockMap_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultVDOTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
