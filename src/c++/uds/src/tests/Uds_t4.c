// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "config.h"
#include "index.h"
#include "indexer.h"
#include "index-session.h"
#include "memory-alloc.h"

static struct block_device *testDevice;

/**********************************************************************/
static void initNullTest(void)
{
  struct uds_index_session *session;
  UDS_ASSERT_ERROR(-EINVAL, uds_create_index_session(NULL));
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
 
  struct uds_parameters *empty;
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(session, &empty));
  vdo_free(empty);

  UDS_ASSERT_ERROR(-EINVAL, uds_open_index(UDS_LOAD, NULL, session));

  struct uds_parameters params = {
    .memory_size = 1,
  };
  UDS_ASSERT_ERROR(-EINVAL, uds_open_index(UDS_LOAD, &params, session));

  params.bdev = testDevice;
  UDS_ASSERT_ERROR(-EINVAL, uds_open_index(UDS_LOAD, &params, NULL));

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void checkMemoryConfig(uds_memory_config_size_t size, uint64_t pages)
{
  struct uds_parameters params = {
    .memory_size = size,
  };
  struct uds_configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  // Peek inside the config and validate it.
  CU_ASSERT_EQUAL((uint64_t) config->geometry->chapters_per_volume *
                  (uint64_t) config->geometry->record_pages_per_chapter,
                  pages);
  uds_free_configuration(config);
}

/**********************************************************************/
static void checkSparseMemoryConfig(uds_memory_config_size_t size,
                                    uint64_t pages)
{
  struct uds_parameters params = {
    .memory_size = size,
    .sparse = true,
  };
  struct uds_configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  // Peek inside the config and validate it.
  CU_ASSERT_EQUAL((uint64_t) config->geometry->chapters_per_volume *
                  (uint64_t) config->geometry->record_pages_per_chapter,
                  pages);
  uds_free_configuration(config);
}

/**********************************************************************/
static void initMemTest(void)
{
  // Too small
  struct uds_parameters params = {
    .memory_size = 0,
  };
  struct uds_configuration *config;
  UDS_ASSERT_ERROR(-EINVAL, uds_make_configuration(&params, &config));

  // Legal small sizes
  checkMemoryConfig(UDS_MEMORY_CONFIG_256MB, 64 * 1024);
  checkMemoryConfig(UDS_MEMORY_CONFIG_512MB, 2 * 64 * 1024);
  checkMemoryConfig(UDS_MEMORY_CONFIG_768MB, 3 * 64 * 1024);
  // Legal large sizes
  unsigned int memGB;
  for (memGB = 1; memGB <= UDS_MEMORY_CONFIG_MAX; memGB++) {
    checkMemoryConfig(memGB, memGB * 256 * 1024);
  }
  // Legal small reduced chapters/volume sizes
  checkMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_256MB, 64 * 1023);
  checkMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_512MB, 2 * 64 * 1023);
  checkMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_768MB, 3 * 64 * 1023);
  // Legal large reduced chapters/volume sizes
  for (memGB = 1 ; memGB <= UDS_MEMORY_CONFIG_MAX; memGB++) {
    checkMemoryConfig(memGB + UDS_MEMORY_CONFIG_REDUCED,
                      memGB * 256 * 1024 - 256);
  }

  // Legal small sizes sparse
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_256MB, 64 * 10240);
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_512MB, 2 * 64 * 10240);
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_768MB, 3 * 64 * 10240);
  // Legal large sizes sparse
  for (memGB = 1; memGB <= UDS_MEMORY_CONFIG_MAX; memGB++) {
    checkSparseMemoryConfig(memGB, memGB * 256 * 10240);
  }
  // Legal small reduced chapters/volume sizes sparse
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_256MB, 64 * 10239);
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_512MB, 2 * 64 * 10239);
  checkSparseMemoryConfig(UDS_MEMORY_CONFIG_REDUCED_768MB, 3 * 64 * 10239);
  // Legal large reduced chapters/volume sizes sparse
  for (memGB = 1 ; memGB <= UDS_MEMORY_CONFIG_MAX; memGB++) {
    checkSparseMemoryConfig(memGB + UDS_MEMORY_CONFIG_REDUCED,
                            memGB * 256 * 10240 - 256);
  }

  // Too big
  params.memory_size = UDS_MEMORY_CONFIG_MAX + 1;
  UDS_ASSERT_ERROR(-EINVAL, uds_make_configuration(&params, &config));
}

/**********************************************************************/
static void checkZoneParameter(unsigned int requested, unsigned int expected)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .zone_count = requested,
    .bdev = testDevice,
  };

  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  // Verify that we got the expected number of zones.
  CU_ASSERT_EQUAL(expected, session->index->zone_count);
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void zoneParameterTest(void)
{
  // A value of zero should get the default zone count, which is half
  // the available cores, from 1 up to MAX_ZONES.
  unsigned int expectedZoneCount = min((unsigned int) MAX_ZONES, max(num_online_cpus() / 2U, 1U));
  checkZoneParameter(0, expectedZoneCount);
  unsigned int z;
  for (z = 1; z <= MAX_ZONES; z++) {
    checkZoneParameter(z, z);
  }
  // Too large should get MAX_ZONES
  checkZoneParameter(MAX_ZONES + 1, MAX_ZONES);
}

/**********************************************************************/
static void createIndexTest(void)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
 };

  /* Make the index */
  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));

  /* Check that UDS_CREATE will clobber the index we just made */
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void reuseIndexTest(void)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };

  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));

  /* Check that the index cannot be reloaded or recreated while it is open */
  UDS_ASSERT_ERROR(-EBUSY, uds_open_index(UDS_LOAD, &params, session));
  UDS_ASSERT_ERROR(-EBUSY, uds_open_index(UDS_CREATE, &params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));

  /* Check that a closed index can be reloaded or recreated */
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_LOAD, &params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void closeIndexTest(void)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .bdev = testDevice,
  };

  /* Make the index */
  struct uds_index_session *session;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));

  /* Try destroying the session without closing the index explicitly. */
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/**********************************************************************/
static void initializerWithBlockDevice(struct block_device *bdev)
{
  testDevice = bdev;
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"initNull"         , initNullTest },
  {"initMem"          , initMemTest },
  {"zoneParameter"    , zoneParameterTest },
  {"createIndex"      , createIndexTest },
  {"reuseIndex"       , reuseIndexTest },
  {"close on destroy" , closeIndexTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "Uds_t4",
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
