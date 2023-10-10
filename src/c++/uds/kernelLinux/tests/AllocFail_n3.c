// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * Test memory allocation failures that happen during the loading of a local
 * index.
 **/

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "uds.h"

static struct block_device *testDevice;

/**********************************************************************/
static size_t getBytesUsed(void)
{
  uint64_t bytesUsed, peakBytesUsed;
  uds_get_memory_stats(&bytesUsed, &peakBytesUsed);
  return bytesUsed;
}

/**********************************************************************/
static void loadTest(void)
{
  // Create and close an index.  This allocates the memory needed for session
  // groups that will persist throughout the test.
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  // Record the number of bytes that have been allocated.
  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(true));
  size_t allocationOverhead = getBytesUsed();

  // Test that loading and closing an index does not leak memory.
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params, indexSession));
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());

  // Test that failing to load an index does not leak memory.
  unsigned int pass = 1;
  bool loop;
  for (loop = true; loop; pass++) {
    albPrint("Loading Pass %u", pass);
    schedule_uds_memory_allocation_failure(pass);
    int result = uds_open_index(UDS_NO_REBUILD, &params, indexSession);
    loop = !uds_allocation_failure_scheduled();
    cancel_uds_memory_allocation_failure();
    if (result == UDS_SUCCESS) {
      UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
    } else {
      UDS_ASSERT_ERROR2(-ENOMEM, -EEXIST, result);
    }
    if (allocationOverhead < getBytesUsed()) {
      log_uds_memory_allocations();
    }
    CU_ASSERT_EQUAL(allocationOverhead, getBytesUsed());
  }
  UDS_ASSERT_SUCCESS(track_uds_memory_allocations(false));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
}

/**********************************************************************/
static void initializerWithBlockDevice(struct block_device *bdev)
{
  testDevice = bdev;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Allocation during load", loadTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "AllocFail_n3",
  .initializerWithBlockDevice = initializerWithBlockDevice,
  .tests                      = tests,
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
