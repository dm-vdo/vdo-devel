// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * Test memory allocation failures that happen during the closing of a local
 * index.
 **/

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "uds.h"

static const char *indexName;

/**********************************************************************/
static size_t getBytesUsed(void)
{
  uint64_t bytesUsed, peakBytesUsed;
  uds_get_memory_stats(&bytesUsed, &peakBytesUsed);
  return bytesUsed;
}

/**********************************************************************/
static void closeSessionTest(void)
{
  // Create and close the index.
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));

  // Record the number of bytes that have been allocated.
  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(true));
  size_t allocationOverhead = getBytesUsed();

  // Test that creating and destroying a session does not leak memory.
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());

  // Test that failing to destory a session does not leak memory.  We loop
  // while we see memory allocation failures during the destruction.
  unsigned int pass = 1;
  bool loop;
  for (loop = true; loop; pass++) {
    albPrint("Closing Pass %u", pass);
    UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
    schedule_uds_memory_allocation_failure(pass);
    int result = uds_destroy_index_session(indexSession);
    loop = !uds_allocation_failure_scheduled();
    cancel_uds_memory_allocation_failure();
    UDS_ASSERT_ERROR2(UDS_SUCCESS, -ENOMEM, result);
    if (allocationOverhead < getBytesUsed()) {
      log_uds_memory_allocations();
    }
    CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());
  }

  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(false));
}

/**********************************************************************/
static void closeIndexTest(void)
{
  // Create and close the index.
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  // Record the number of bytes that have been allocated.
  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(true));
  size_t allocationOverhead = getBytesUsed();

  // Test that creating and closing an index does not leak memory.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());

  // Test that failing to close an index does not leak memory.  We loop while
  // we see memory allocation failures during the closing.
  unsigned int pass = 1;
  bool loop;
  for (loop = true; loop; pass++) {
    albPrint("Closing Pass %u", pass);
    UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
    schedule_uds_memory_allocation_failure(pass);
    int result = uds_close_index(indexSession);
    loop = !uds_allocation_failure_scheduled();
    cancel_uds_memory_allocation_failure();
    UDS_ASSERT_ERROR2(UDS_SUCCESS, -ENOMEM, result);
    if (allocationOverhead < getBytesUsed()) {
      log_uds_memory_allocations();
    }
    CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());
  }

  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(false));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
}

/**********************************************************************/
static void initializerWithIndexName(const char *in)
{
  indexName = in;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Close session after create", closeSessionTest },
  {"Close index after create",   closeIndexTest   },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "AllocFail_n2",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
