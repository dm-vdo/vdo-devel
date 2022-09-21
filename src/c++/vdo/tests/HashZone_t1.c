/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/prandom.h>

#include "numeric.h"

#include "dedupe.h"
#include "thread-config.h"
#include "vdo.h"

#include "dedupeContext.h"
#include "hashZone.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**
 * Create a random block name.
 *
 * @param name  the resulting random block name
 **/
static void createRandomBlockName(struct uds_record_name *name)
{
  prandom_bytes(name->name, UDS_RECORD_NAME_SIZE);
}

/**
 * Verify that vdo_select_hash_zone evenly distributes record names among all
 * the hash zones.
 *
 * @param vdo       The vdo containing the hash zones
 * @param hashZone  The configured number of hash zones
 **/
static void verifySelectHashZone(struct vdo *vdo, thread_count_t hashZones)
{
  unsigned int histogram[hashZones];
  memset(histogram, 0, sizeof(histogram));

  struct uds_record_name name;
  createRandomBlockName(&name);

  // Since we only use the first byte to select hash zone, we can easily
  // test all possible values.
  enum { NAME_COUNT = 256 };
  for (unsigned int selector = 0; selector < NAME_COUNT; selector++) {
    // XXX factor ala hashUtils
    name.name[0] = selector;
    struct hash_zone *zone = vdo_select_hash_zone(vdo->hash_zones, &name);
    histogram[zone->zone_number] += 1;
    // Check that we get the same name if we ask again, which should catch the
    // unlikely case of even but non-repeatable distribution, such as a rotor.
    CU_ASSERT_PTR_EQUAL(zone, vdo_select_hash_zone(vdo->hash_zones, &name));
  }

  // The names should have been evenly distributed among all the zones.
  unsigned int minimum = INT_MAX;
  unsigned int maximum = 0;
  unsigned int total = 0;
  for (zone_count_t i = 0; i < ARRAY_SIZE(histogram); i++) {
    if (i < hashZones) {
      total += histogram[i];
      minimum = min(minimum, histogram[i]);
      maximum = max(maximum, histogram[i]);
    } else {
      CU_ASSERT_EQUAL(0, histogram[i]);
    }
  }
  CU_ASSERT_EQUAL(NAME_COUNT, total);

  // An even distibution will be all equal, or at most differ by one.
  CU_ASSERT_TRUE((maximum - minimum) <= 1);
}

/**
 * Change the number of hash zone threads configured and reload the VDO
 * so the change takes effect.
 *
 * @param hashZoneCount  The desired number of hash zones
 **/
static void reconfigureHashZones(zone_count_t hashZoneCount)
{
  struct device_config config = getTestConfig().deviceConfig;
  config.thread_counts.hash_zones = hashZoneCount;
  reloadVDO(config);
}

/**
 * Fully exercise vdo_select_hash_zone for all likely (and some unlikely) hash
 * zone configurations.
 **/
static void testSelectVdoHashZone(void)
{
  // It's unlikely there will ever be even 10 hash zone threads, but it's
  // cheap enough to check.
  for (thread_count_t hashZones = 1; hashZones < 16; hashZones++) {
    reconfigureHashZones(hashZones);
    CU_ASSERT_EQUAL(hashZones, vdo->thread_config->hash_zone_count);
    verifySelectHashZone(vdo, hashZones);
  }
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test vdo_select_hash_zone", testSelectVdoHashZone },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "HashZone_t1",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultVDOTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
