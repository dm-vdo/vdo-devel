/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdlib.h>

#include "albtest.h"

#include "status-codes.h"
#include "vdoAsserts.h"

/**
 * Trivial test for VDO status codes.
 **/
static void trivialTest(void)
{
  char buf[1000];

  // UDS

  CU_ASSERT_STRING_EQUAL(uds_string_error_name(UDS_NO_INDEX, buf,
                                               sizeof(buf)),
                         "UDS_NO_INDEX");

  CU_ASSERT_CONTAINS_SUBSTRING(uds_string_error(UDS_NO_INDEX, buf,
                                                sizeof(buf)),
                               "No index found");

  // VDO

  CU_ASSERT_STRING_EQUAL(uds_string_error_name(VDO_NO_SPACE, buf, sizeof(buf)),
                         "VDO_NO_SPACE");

  CU_ASSERT_CONTAINS_SUBSTRING(uds_string_error(VDO_NO_SPACE, buf,
                                                sizeof(buf)),
                               "Out of space");

}

/**********************************************************************/

static CU_TestInfo statusCodesTests[] = {
  { "trivial", trivialTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo statusCodesSuite = {
  .name                     = "Trivial statusCodes tests (StatusCodes_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = statusCodesTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &statusCodesSuite;
}
