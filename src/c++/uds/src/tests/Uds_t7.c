// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * This suite includes tests of the new block context interface.
 **/

#include "albtest.h"
#include "assertions.h"
#include "uds.h"

static struct uds_index_session *indexSession;

/**********************************************************************/
static void argumentsTest(void)
{
  UDS_ASSERT_ERROR(-EINVAL, uds_get_index_parameters(indexSession, NULL));
  UDS_ASSERT_ERROR(-EINVAL, uds_get_index_stats(indexSession, NULL));
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {

  {"Invalid Arguments", argumentsTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                   = "Uds_t7",
  .initializerWithSession = initializerWithSession,
  .tests                  = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
