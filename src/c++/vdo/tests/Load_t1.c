/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "encodings.h"
#include "types.h"

#include "userVDO.h"
#include "vdoConfig.h"

#include "asyncLayer.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void testBadSuperBlockVersion(void)
{
  stopVDO();

  // Perturb the superblock version.
  char buffer[VDO_BLOCK_SIZE];
  physical_block_number_t superBlockLocation = getSuperBlockLocation();
  VDO_ASSERT_SUCCESS(layer->reader(layer, superBlockLocation, 1, buffer));

  struct header *rawHeader = (struct header *) (void *) buffer;
  rawHeader->version.major_version += 3;
  rawHeader->version.minor_version += 29;
  VDO_ASSERT_SUCCESS(layer->writer(layer, superBlockLocation, 1, buffer));
  startVDOExpectError(vdo_map_to_system_error(VDO_UNSUPPORTED_VERSION));
}

/**********************************************************************/
static void testMismatchedNonce(void)
{
  stopVDO();

  // Perturb the nonce in the geometry block.
  struct volume_geometry geometry;
  VDO_ASSERT_SUCCESS(loadVolumeGeometry(layer, &geometry));
  geometry.nonce++;
  VDO_ASSERT_SUCCESS(writeVolumeGeometry(layer, &geometry));
  startVDOExpectError(vdo_map_to_system_error(VDO_BAD_NONCE));
}

/**********************************************************************/
static void testMismatchedReleaseVersion(void)
{
  stopVDO();

  // Perturb the unused field (nee release version) in the geometry block.
  struct volume_geometry geometry;
  VDO_ASSERT_SUCCESS(loadVolumeGeometry(layer, &geometry));
  geometry.unused++;
  VDO_ASSERT_SUCCESS(writeVolumeGeometry(layer, &geometry));
  startVDO(VDO_CLEAN);
}

/**********************************************************************/
static void testReadOnlyDevice(void)
{
  stopVDO();

  // Start and stop the VDO while the device is in read-only mode.
  setAsyncLayerReadOnly(true);
  startReadOnlyVDO(VDO_CLEAN);
  stopVDO();
  setAsyncLayerReadOnly(false);
  startVDO(VDO_CLEAN);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "load bad super block version",    testBadSuperBlockVersion     },
  { "load mismatched nonce",           testMismatchedNonce          },
  { "load mismatched release version", testMismatchedReleaseVersion },
  { "load on a read-only device",      testReadOnlyDevice           },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "Load_t1",
  .initializer = initializeDefaultVDOTest,
  .cleaner     = tearDownVDOTest,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
