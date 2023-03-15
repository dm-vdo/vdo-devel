/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdint.h>

#include "albtest.h"

#include "constants.h"
#include "physical-zone.h"
#include "slab-depot.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

#include "logger.h"

/**
 * Change the number of physical zone threads configured and reload the VDO
 * so the change takes effect.
 *
 * @param physicalZoneCount  The desired number of physical zones
 **/
static void reconfigurePhysicalZones(zone_count_t physicalZoneCount)
{
  struct device_config config = getTestConfig().deviceConfig;
  config.thread_counts.physical_zones = physicalZoneCount;
  reloadVDO(config);
}

/**
 * Assert that vdo_get_physical_zone returns VDO_OUT_OF_RANGE for a PBN.
 *
 * @param vdo     The test vdo
 * @param badPBN  The out-of-range PBN
 **/
static void assertInvalidPBN(struct vdo *vdo, physical_block_number_t badPBN)
{
  struct physical_zone *zone;
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE, vdo_get_physical_zone(vdo, badPBN, &zone));
}

/**
 * Verify that vdo_get_physical_zone accurately and safely maps PBNs to zones.
 *
 * @param physicalZone  The number of physical zones to configure and test
 **/
static void verifyGetPhysicalZone(thread_count_t zoneCount)
{
  reconfigurePhysicalZones(zoneCount);

  struct vdo_config config = getTestConfig().config;

  CU_ASSERT_EQUAL(zoneCount, vdo->thread_config->physical_zone_count);

  // Check that the zones are all initialized and are who they think they are.
  for (zone_count_t i = 0; i < zoneCount; i++) {
    CU_ASSERT_EQUAL(i, vdo->physical_zones->zones[i].zone_number);
  }

  // Slabs are laid out sequentially on disk, so keep track of which slab
  // we're currently finding blocks in as we interate over PBNs.
  struct vdo_slab      *currentSlab       = NULL;
  struct physical_zone *currentZone       = NULL;
  block_count_t         dataBlocksPerSlab = vdo->depot->slab_config.data_blocks;

  // Keep a count of the number of slebs associated with each zone so we can
  // check that they're as evenly distributed as possible.
  slab_count_t slabsPerZone[zoneCount];
  memset(slabsPerZone, 0, sizeof(slabsPerZone));

  // This code tries to not assume too much about slab layout, but it's
  // simpler here to expect that data blocks in adjacent slabs don't touch.
  bool inSlab = false;

  uds_log_info("checking %d zones with %zu blocks in %d slabs",
               zoneCount, config.physical_blocks, vdo->depot->slab_count);

  for (physical_block_number_t pbn = 0; pbn < config.physical_blocks; pbn++) {
    struct physical_zone *zone;
    int result = vdo_get_physical_zone(vdo, pbn, &zone);
    if (result != VDO_SUCCESS) {
      CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE, result);
      if (inSlab) {
        // We seem to have fallen off the end of a slab.
        CU_ASSERT_EQUAL(currentSlab->start + dataBlocksPerSlab, pbn);

        inSlab = false;
      }
      continue;
    }
    if (pbn == VDO_ZERO_BLOCK) {
      CU_ASSERT_PTR_NULL(zone);
      continue;
    }

    CU_ASSERT_PTR_NOT_NULL(zone);

    if (inSlab) {
      CU_ASSERT_PTR_EQUAL(currentZone, zone);
      continue;
    }

    // We've reached the start of a run of data blocks in the next slab.
    inSlab = true;
    struct vdo_slab *slab = vdo_get_slab(vdo->depot, pbn);
    CU_ASSERT_PTR_NOT_NULL(slab);

    // Verify our assumption that data blocks in a slab are continuous and
    // that slabs are sequentially numbered.
    if (currentSlab == NULL) {
      CU_ASSERT_EQUAL(0, slab->slab_number);
    } else {
      CU_ASSERT_EQUAL(currentSlab->slab_number + 1, slab->slab_number);
    }

    currentSlab = slab;
    currentZone = zone;
    slabsPerZone[zone->zone_number] += 1;

    // We've just found what should be the first data block in the zone.
    CU_ASSERT_EQUAL(slab->start, pbn);
  }

  // Make sure we iterated over all the slabs. Since they're sequential, we
  // just have to check that we reached the end.
  CU_ASSERT_EQUAL(currentSlab->slab_number, vdo->depot->slab_count - 1);

  // Make sure we saw every zone and that the slabs were evenly distributed.
  slab_count_t min = slabsPerZone[0];
  slab_count_t max = slabsPerZone[0];
  for (zone_count_t i = 1; i < zoneCount; i++) {
    slab_count_t seen = slabsPerZone[i];
    if (min > seen) {
      min = seen;
    } else if (max < seen) {
      max = seen;
    }
  }
  if ((vdo->depot->slab_count % zoneCount) == 0) {
    CU_ASSERT_EQUAL(min, max);
  } else {
    CU_ASSERT_EQUAL(min + 1, max);
  }

  // It's too expensive to check every possible bogus PBN value, but we can at
  // least check some likely suspects that could mess things up.

  // block_map_entry encodes 36 bits of PBN, so check boundary cases around
  // that.
  physical_block_number_t maxPackedPBN = MAXIMUM_VDO_PHYSICAL_BLOCKS - 1;
  assertInvalidPBN(vdo, maxPackedPBN - 1);
  assertInvalidPBN(vdo, maxPackedPBN);
  assertInvalidPBN(vdo, maxPackedPBN + 1);
  assertInvalidPBN(vdo, maxPackedPBN + 2);

  // These are selected because of potential integer overflow/wrap to zero.
  assertInvalidPBN(vdo, config.physical_blocks);
  assertInvalidPBN(vdo, config.physical_blocks + 1);
  assertInvalidPBN(vdo, (physical_block_number_t) -1);
  assertInvalidPBN(vdo, (physical_block_number_t) U64_MAX);
  assertInvalidPBN(vdo, (physical_block_number_t) S64_MAX);
  assertInvalidPBN(vdo, (physical_block_number_t) S64_MAX + 1);
  assertInvalidPBN(vdo, (physical_block_number_t) U32_MAX);
  assertInvalidPBN(vdo, (physical_block_number_t) U32_MAX + 1);
  assertInvalidPBN(vdo, (physical_block_number_t) S32_MAX);
  assertInvalidPBN(vdo, (physical_block_number_t) S32_MAX + 1);
}

/**
 * Test that vdo_get_physical_zone returns the correct slab for valid data
 * PBNs and doesn't put the VDO into read-only mode on all other PBNs.
 **/
static void testGetVdoPhysicalZone(void)
{
  // It's unlikely there will ever be even 10 physical zone threads, but it's
  // cheap enough to check a few high counts.
  zone_count_t threadCounts[] = { 1, 2, 3, 4, 5, 6, 11, 12, 16 };
  for (unsigned int i = 0; i < ARRAY_SIZE(threadCounts); i++) {
    verifyGetPhysicalZone(threadCounts[i]);
    CU_ASSERT_FALSE(vdo_in_read_only_mode(vdo));
  }
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test vdo_get_physical_zone", testGetVdoPhysicalZone },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "PhysicalZone_t1",
  .initializer = initializeDefaultVDOTest,
  .cleaner     = tearDownVDOTest,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
