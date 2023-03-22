/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stddef.h>

#include "albtest.h"

#include "types.h"
#include "vdo.h"

#include "testParameters.h"
#include "vdoAsserts.h"

/**
 * Get the thread name for a specified thread from a thread config and assert
 * that it matches the expected format.
 *
 * @param config          The thread config to query
 * @param id              The thread ID to check
 * @param baseName        The expected name or prefix of the expected name
 * @param expectedSuffix  If non-negative, the number to append to the
 *                        base name to form the expected name
 **/
static void assertThreadName(const struct thread_config *config,
                             thread_id_t                 id,
                             const char                 *baseName,
                             int                         expectedSuffix)
{
  char expectedName[64];
  if (expectedSuffix < 0) {
    snprintf(expectedName, sizeof(expectedName), "%s", baseName);
  } else {
    snprintf(expectedName, sizeof(expectedName),
             "%s%d", baseName, expectedSuffix);
  }

  char name[64];
  get_thread_name(config, id, name, sizeof(name));
  CU_ASSERT_STRING_EQUAL(expectedName, name);

  // Make sure we don't overflow short buffers.
  get_thread_name(config, id, name, 1);
  CU_ASSERT_STRING_EQUAL("", name);

  get_thread_name(config, id, name, 2);
  CU_ASSERT_EQUAL(expectedName[0], name[0]);
  CU_ASSERT_EQUAL('\0', name[1]);
}

/**********************************************************************/
static void testOneThreadConfig(void)
{
  struct thread_count_config counts = {
    .bio_ack_threads = 1,
    .bio_threads = DEFAULT_VDO_BIO_SUBMIT_QUEUE_COUNT,
    .bio_rotation_interval = DEFAULT_VDO_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
    .cpu_threads = 1,
  };
  struct thread_config config;
  memset(&config, 0, sizeof(struct thread_config));
  VDO_ASSERT_SUCCESS(initialize_thread_config(counts, &config));

  CU_ASSERT_EQUAL(1, config.logical_zone_count);
  CU_ASSERT_EQUAL(1, config.physical_zone_count);
  CU_ASSERT_EQUAL(1, config.hash_zone_count);

  // Thread zero services all base threads.
  CU_ASSERT_EQUAL(0, config.admin_thread);
  CU_ASSERT_EQUAL(0, config.journal_thread);
  CU_ASSERT_EQUAL(0, config.packer_thread);
  CU_ASSERT_EQUAL(0, config.logical_threads[0]);
  CU_ASSERT_EQUAL(0, config.physical_threads[0]);
  CU_ASSERT_EQUAL(0, config.hash_zone_threads[0]);

  assertThreadName(&config, 0, "reqQ", -1);

  thread_id_t baseID = 1;
  CU_ASSERT_EQUAL(config.dedupe_thread, baseID);
  assertThreadName(&config, baseID++, "dedupeQ", -1);
  CU_ASSERT_EQUAL(config.bio_ack_thread, baseID);
  assertThreadName(&config, baseID++, "ackQ", -1);
  CU_ASSERT_EQUAL(config.cpu_thread, baseID);
  assertThreadName(&config, baseID++, "cpuQ", -1);

  for (zone_count_t zone = 0; zone < config.bio_thread_count; zone++) {
    assertThreadName(&config, baseID++, "bioQ", zone);
  }

  CU_ASSERT_EQUAL(config.thread_count, baseID);
  uninitialize_thread_config(&config);
}

/**********************************************************************/
static void testBasicThreadConfig(void)
{
  enum {
    LOGICAL_ZONES    = 4,
    PHYSICAL_ZONES   = 3,
    HASH_ZONES       = 2,
    BIO_THREADS      = 2,
    BIO_ACK_THREADS  = 3,
    JOURNAL_THREAD   = 0,
    PACKER_THREAD    = 1,
    LOGICAL_THREAD_0 = 2,
  };
  struct thread_count_config counts = {
    .logical_zones = LOGICAL_ZONES,
    .physical_zones = PHYSICAL_ZONES,
    .hash_zones = HASH_ZONES,
    .bio_threads = BIO_THREADS,
    .bio_ack_threads = BIO_ACK_THREADS,
  };
  struct thread_config config;
  memset(&config, 0, sizeof(struct thread_config));
  VDO_ASSERT_SUCCESS(initialize_thread_config(counts, &config));

  CU_ASSERT_EQUAL(LOGICAL_ZONES, config.logical_zone_count);
  CU_ASSERT_EQUAL(PHYSICAL_ZONES, config.physical_zone_count);
  CU_ASSERT_EQUAL(HASH_ZONES, config.hash_zone_count);
  CU_ASSERT_EQUAL(BIO_THREADS, config.bio_thread_count);

  // Thread zero doubles as the admin and journal thread.
  CU_ASSERT_EQUAL(JOURNAL_THREAD, config.admin_thread);
  CU_ASSERT_EQUAL(JOURNAL_THREAD, config.journal_thread);
  assertThreadName(&config, JOURNAL_THREAD, "journalQ", -1);

  CU_ASSERT_EQUAL(PACKER_THREAD, config.packer_thread);
  assertThreadName(&config, PACKER_THREAD, "packerQ", -1);

  thread_id_t baseID = LOGICAL_THREAD_0;
  for (zone_count_t zone = 0; zone < LOGICAL_ZONES; zone++) {
    CU_ASSERT_EQUAL(baseID + zone, config.logical_threads[zone]);
    assertThreadName(&config, baseID + zone, "logQ", zone);
  }
  baseID += LOGICAL_ZONES;

  for (zone_count_t zone = 0; zone < PHYSICAL_ZONES; zone++) {
    CU_ASSERT_EQUAL(baseID + zone, config.physical_threads[zone]);
    assertThreadName(&config, baseID + zone, "physQ", zone);
  }
  baseID += PHYSICAL_ZONES;

  for (zone_count_t zone = 0; zone < HASH_ZONES; zone++) {
    CU_ASSERT_EQUAL(baseID + zone, config.hash_zone_threads[zone]);
    assertThreadName(&config, baseID + zone, "hashQ", zone);
  }
  baseID += HASH_ZONES;

  CU_ASSERT_EQUAL(config.dedupe_thread, baseID);
  assertThreadName(&config, baseID++, "dedupeQ", -1);
  CU_ASSERT_EQUAL(config.bio_ack_thread, baseID);
  assertThreadName(&config, baseID++, "ackQ", -1);
  CU_ASSERT_EQUAL(config.cpu_thread, baseID);
  assertThreadName(&config, baseID++, "cpuQ", -1);

  for (zone_count_t zone = 0; zone < BIO_THREADS; zone++) {
    assertThreadName(&config, baseID + zone, "bioQ", zone);
  }
  baseID += BIO_THREADS;

  CU_ASSERT_EQUAL(config.thread_count, baseID);

  uninitialize_thread_config(&config);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "test the single-thread configuration",       testOneThreadConfig   },
  { "test a basic multiple-thread configuration", testBasicThreadConfig },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "struct thread_config tests (ThreadConfig_t1)",
  .initializer = NULL,
  .cleaner     = NULL,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
