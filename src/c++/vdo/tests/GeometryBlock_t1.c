/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "constants.h"
#include "volume-geometry.h"

#include "physicalLayer.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  MAGIC_NUMBER_SIZE = 8,
};

static nonce_t NONCE     = 0x1020304beef51ab5;
static uuid_t  TEST_UUID = "fake\0uuid hares";

/*
 * A captured encoding of the geometry block version 4.0 created by
 * encodingTest_4_0(). This is used to check that the encoding format hasn't
 * changed and is platform-independent.
 */
static u8 EXPECTED_GEOMETRY_4_0_ENCODING[] =
  {
    0x64, 0x6d, 0x76, 0x64, 0x6f, 0x30, 0x30, 0x31, // magic = "dmvdo001"
    0x05, 0x00, 0x00, 0x00,                         // header.id = GEOMETRY
    0x04, 0x00, 0x00, 0x00,                         //   .majorVersion = 4
    0x00, 0x00, 0x00, 0x00,                         //   .minorVersion = 0
    0x5d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   .size = 93
    0x1d, 0x1c, 0x1b, 0x1a,                         // release = 0x1a1b1c1d
    0xb5, 0x1a, 0xf5, 0xee, 0x4b, 0x30, 0x20, 0x10, // nonce = NONCE
    0x66, 0x61, 0x6b, 0x65, 0x00, 0x75, 0x75, 0x69, // uuid = TEST_UUID
    0x64, 0x20, 0x68, 0x61, 0x72, 0x65, 0x73, 0x00, //   ...  TEST_UUID
                                                    // region
    0x00, 0x00, 0x00, 0x00,                         //   .id = VDO_INDEX_REGION
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, //   .start  = 0x212223...
                                                    // region
    0x01, 0x00, 0x00, 0x00,                         //   .id = VDO_DATA_REGION
    0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, //   .start  = 0x313233...
                                                    // index_config
    0x4d, 0x4c, 0x4b, 0x4a,                         //   mem = 0x4a4b4c4d
    0x00, 0x00, 0x00, 0x00,                         //   (unused)
    0x01,                                           //   sparse = true
    0x39, 0x34, 0xe4, 0x3e,                         // checksum = 0x3ee43439
  };

/*
 * A captured encoding of the geometry block version 5.0 created by
 * encodingTest_5_0(). This is used to check that the encoding format hasn't
 * changed and is platform-independent.
 */
static u8 EXPECTED_GEOMETRY_5_0_ENCODING[] =
  {
    0x64, 0x6d, 0x76, 0x64, 0x6f, 0x30, 0x30, 0x31, // magic = "dmvdo001"
    0x05, 0x00, 0x00, 0x00,                         // header.id = GEOMETRY
    0x05, 0x00, 0x00, 0x00,                         //   .majorVersion = 5
    0x00, 0x00, 0x00, 0x00,                         //   .minorVersion = 0
    0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   .size = 101
    0x1d, 0x1c, 0x1b, 0x1a,                         // release = 0x1a1b1c1d
    0xb5, 0x1a, 0xf5, 0xee, 0x4b, 0x30, 0x20, 0x10, // nonce = NONCE
    0x66, 0x61, 0x6b, 0x65, 0x00, 0x75, 0x75, 0x69, // uuid = TEST_UUID
    0x64, 0x20, 0x68, 0x61, 0x72, 0x65, 0x73, 0x00, //   ...  TEST_UUID
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, // bio_offset = 0x111213...
                                                    // region
    0x00, 0x00, 0x00, 0x00,                         //   .id = VDO_INDEX_REGION
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, //   .start  = 0x212223...
                                                    // region
    0x01, 0x00, 0x00, 0x00,                         //   .id = VDO_DATA_REGION
    0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31, //   .start  = 0x313233...
                                                    // index_config
    0x4d, 0x4c, 0x4b, 0x4a,                         //   mem = 0x4a4b4c4d
    0x00, 0x00, 0x00, 0x00,                         //   (unused)
    0x01,                                           //   sparse = true
    0xd6, 0x99, 0x9d, 0x04,                         // checksum = 0x049d99d6
  };

/**********************************************************************/
static void encodingTest_4_0(void)
{
  struct volume_geometry geometry;
  VDO_ASSERT_SUCCESS(vdo_initialize_volume_geometry(NONCE, &TEST_UUID, NULL,
                                                    &geometry));
  // Save the release version so we can use a valid value later.
  release_version_number_t savedRelease = geometry.release_version;

  // Fill the geometry fields with bogus values that will test endianness.
  geometry.release_version                   = 0x1a1b1c1d;
  geometry.regions[0].start_block            = 0x2122232425262728;
  geometry.regions[1].start_block            = 0x3132333435363738;
  geometry.index_config.mem                  = 0x4a4b4c4d;
  geometry.index_config.sparse               = true;

  // Encode and write the volume_geometry for version 4.
  PhysicalLayer *layer = getSynchronousLayer();
  VDO_ASSERT_SUCCESS(vdo_write_volume_geometry_with_version(layer,
                                                            &geometry, 4));

  // Read and compare it to the expected byte sequence.
  char block[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, 1, block));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_GEOMETRY_4_0_ENCODING,
                         block, sizeof(EXPECTED_GEOMETRY_4_0_ENCODING));

  // Can't load the bogus release version, so re-encode with the saved one.
  geometry.release_version = savedRelease;
  VDO_ASSERT_SUCCESS(vdo_write_volume_geometry_with_version(layer,
                                                            &geometry, 4));

  // Read, decode, and compare the decoded volume_geometry.
  struct volume_geometry decoded;
  VDO_ASSERT_SUCCESS(vdo_load_volume_geometry(getSynchronousLayer(),
                                              &decoded));
  UDS_ASSERT_EQUAL_BYTES(&geometry, &decoded, sizeof(decoded));
}

/**********************************************************************/
static void encodingTest_5_0(void)
{
  struct volume_geometry geometry;
  VDO_ASSERT_SUCCESS(vdo_initialize_volume_geometry(NONCE, &TEST_UUID, NULL,
                                                    &geometry));
  // Save the release version so we can use a valid value later.
  release_version_number_t savedRelease = geometry.release_version;

  // Fill the geometry fields with bogus values that will test endianness.
  geometry.release_version                   = 0x1a1b1c1d;
  geometry.bio_offset                        = 0x1112131415161718;
  geometry.regions[0].start_block            = 0x2122232425262728;
  geometry.regions[1].start_block            = 0x3132333435363738;
  geometry.index_config.mem                  = 0x4a4b4c4d;
  geometry.index_config.sparse               = true;

  // Encode and write the VolumeGeometry for version 5_0.
  PhysicalLayer *layer = getSynchronousLayer();
  VDO_ASSERT_SUCCESS(vdo_write_volume_geometry(layer, &geometry));

  // Read and compare it to the expected byte sequence for version 5_0.
  char block[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, 1, block));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_GEOMETRY_5_0_ENCODING,
                         block, sizeof(EXPECTED_GEOMETRY_5_0_ENCODING));

  // Can't load the bogus release version, so re-encode with the saved one.
  geometry.release_version = savedRelease;
  VDO_ASSERT_SUCCESS(vdo_write_volume_geometry(layer, &geometry));

  // Read, decode, and compare the decoded volume_geometry.
  struct volume_geometry decoded;
  VDO_ASSERT_SUCCESS(vdo_load_volume_geometry(getSynchronousLayer(),
                                              &decoded));
  UDS_ASSERT_EQUAL_BYTES(&geometry, &decoded, sizeof(decoded));
}

/**********************************************************************/
static void assertRegionIs(struct volume_region  *region,
                           enum volume_region_id  id,
                           uint64_t               startBlock)
{
  CU_ASSERT_EQUAL(region->id,          id);
  CU_ASSERT_EQUAL(region->start_block, startBlock);
}

/**********************************************************************/
static void basicTest(void)
{
  struct volume_geometry geometry;
  VDO_ASSERT_SUCCESS(vdo_initialize_volume_geometry(NONCE, &TEST_UUID, NULL,
                                                    &geometry));
  VDO_ASSERT_SUCCESS(vdo_write_volume_geometry(getSynchronousLayer(),
                                               &geometry));
  VDO_ASSERT_SUCCESS(vdo_load_volume_geometry(getSynchronousLayer(),
                                              &geometry));
  CU_ASSERT_EQUAL(geometry.nonce, NONCE);
  assertRegionIs(&geometry.regions[0], VDO_INDEX_REGION, 1);
  assertRegionIs(&geometry.regions[1], VDO_DATA_REGION,  1);

  // Preserve the original geometry block.
  char geometryBlock[VDO_BLOCK_SIZE];
  char buffer[VDO_BLOCK_SIZE];
  PhysicalLayer *layer = getSynchronousLayer();
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, 1, geometryBlock));

  // Decode the GeometryBlock header.
  struct header header;
  size_t offset = MAGIC_NUMBER_SIZE;
  vdo_decode_header((u8 *) geometryBlock, &offset, &header);

  // Try corrupting the magic number.
  memcpy(buffer, geometryBlock, VDO_BLOCK_SIZE);
  buffer[0] = !buffer[0];
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, 1, buffer));
  CU_ASSERT_EQUAL(vdo_load_volume_geometry(getSynchronousLayer(), &geometry),
                  VDO_BAD_MAGIC);

  // Try corrupting the header.
  memcpy(buffer, geometryBlock, VDO_BLOCK_SIZE);
  buffer[MAGIC_NUMBER_SIZE] = !buffer[MAGIC_NUMBER_SIZE];
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, 1, buffer));
  CU_ASSERT_EQUAL(vdo_load_volume_geometry(getSynchronousLayer(), &geometry),
                  VDO_INCORRECT_COMPONENT);

  // Try faking a different release version.
  memcpy(buffer, geometryBlock, VDO_BLOCK_SIZE);
  buffer[VDO_ENCODED_HEADER_SIZE] = !buffer[VDO_ENCODED_HEADER_SIZE];
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, 1, buffer));
  CU_ASSERT_EQUAL(vdo_load_volume_geometry(getSynchronousLayer(), &geometry),
                  VDO_UNSUPPORTED_VERSION);

  // Try corrupting the checksum.
  memcpy(buffer, geometryBlock, VDO_BLOCK_SIZE);
  buffer[header.size - 1] = 255 - buffer[header.size - 1];
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, 1, buffer));
  CU_ASSERT_EQUAL(vdo_load_volume_geometry(getSynchronousLayer(), &geometry),
                  VDO_CHECKSUM_MISMATCH);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Saves and loads", basicTest        },
  { "Encoding v4_0",   encodingTest_4_0 },
  { "Encoding v5_0",   encodingTest_5_0 },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Geometry block tests (GeometryBlock_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultBasicTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
