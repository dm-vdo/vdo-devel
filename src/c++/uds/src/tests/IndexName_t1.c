// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * IndexName_t1 tests the various index names that it is possible to use.
 **/

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"

static uint64_t size;
static struct uds_parameters baseParameters;

/**********************************************************************/
static void testBad(struct uds_parameters *params)
{
  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_ERROR(-ENOSPC, uds_open_index(UDS_CREATE, params, session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void testGood(struct uds_parameters *params)
{
  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void baseTest(void)
{
  testGood(&baseParameters);
}

/**********************************************************************/
static void sizeTest(void)
{
  // Test with the correct index size
  struct uds_parameters params = baseParameters;
  params.size = size;
  testGood(&params);

  // Test with index size too small
  params.size = size - 1;
  testBad(&params);
}

/**********************************************************************/
static void offsetTest(void)
{
  // Test with an index offset
  struct uds_parameters params = baseParameters;
  params.offset = size;
  testGood(&params);
}

/**********************************************************************/
static void sizeOffsetTest(void)
{
  // Test with an index offset
  struct uds_parameters params = baseParameters;
  params.size = size;
  params.offset = size;
  testGood(&params);
}

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  struct uds_parameters parameters = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = name,
  };
  baseParameters = parameters;
  UDS_ASSERT_SUCCESS(uds_compute_index_size(&parameters, &size));
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "base",        baseTest },
  { "size",        sizeTest },
  { "offset",      offsetTest },
  { "size+offset", sizeOffsetTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "IndexName_t1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
