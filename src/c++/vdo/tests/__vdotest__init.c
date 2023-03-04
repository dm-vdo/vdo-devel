/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdlib.h>

#include "albtest.h"
#include "assertions.h"

#include "status-codes.h"

#include "mutexUtils.h"
#include "vdoTestBase.h"

/**********************************************************************/

static CU_TestDirInfo vdoTestDirInfo = {
  .initializerWithArguments = NULL,
  .initializer              = initializeVDOTestBase,
  .cleaner                  = tearDownVDOTestBase,
};

CU_TestDirInfo *initializeTestDirectory(void)
{
  return &vdoTestDirInfo;
}
