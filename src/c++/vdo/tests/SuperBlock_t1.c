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

#include "encodings.h"
#include "types.h"

#include "asyncLayer.h"
#include "userVDO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // By measurement.
  SUPER_BLOCK_PAYLOAD_SIZE = 418,
  SUPER_BLOCK_SIZE =  VDO_ENCODED_HEADER_SIZE + SUPER_BLOCK_PAYLOAD_SIZE,
  CHECKSUM_OFFSET = SUPER_BLOCK_SIZE - sizeof(u32),
};

/*
 * The expected encoding of the super block version 12.0 header. This is used to test that the
 * encoding format hasn't changed and is platform-independent.
 */
static u8 EXPECTED_SUPERBLOCK_12_0_ENCODED_HEADER[] =
  {
                                                    // header
    0x00, 0x00, 0x00, 0x00,                         //   .id = VDO_SUPER_BLOCK
    0x0C, 0x00, 0x00, 0x00,                         //   .majorVersion = 12
    0x00, 0x00, 0x00, 0x00,                         //   .minorVersion = 0
    0xa2, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   .size = SUPER_BLOCK_PAYLOAD_SIZE (418)
  };

/**********************************************************************/
static void testSuperBlock(void)
{
  // Test set-up will have formatted and started the vdo confirming that what the synchronous save
  // has written is intelligible to the asynchronous load.

  // Check the header.
  char block[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, getSuperBlockLocation(), 1, block));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_SUPERBLOCK_12_0_ENCODED_HEADER, block, VDO_ENCODED_HEADER_SIZE);

  // Stop the VDO, confirm that the super block modified as the vdo state will have changed from
  // VDO_NEW to VDO_CLEAN.
  stopVDO();
  char loaded[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, getSuperBlockLocation(), 1, loaded));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_SUPERBLOCK_12_0_ENCODED_HEADER, loaded, VDO_ENCODED_HEADER_SIZE);
  UDS_ASSERT_NOT_EQUAL_BYTES(block, loaded, SUPER_BLOCK_SIZE);

  // Confirm that synchronous load can read the modified super block.
  VDO_ASSERT_SUCCESS(vdo_decode_super_block((u8 *) &loaded));

  // Break the checksum and confirm that decode/load fails.
  u8 checksumByte = loaded[CHECKSUM_OFFSET];
  memset(loaded + CHECKSUM_OFFSET, ++checksumByte, 1);
  CU_ASSERT_EQUAL(VDO_CHECKSUM_MISMATCH, vdo_decode_super_block((u8 *) &loaded));
  VDO_ASSERT_SUCCESS(layer->writer(layer, getSuperBlockLocation(), 1, loaded));
  setStartStopExpectation(-EIO);
  startAsyncLayer(getTestConfig(), true);
}

/**********************************************************************/

static CU_TestInfo superBlockTests[] = {
  { "test super block save and load", testSuperBlock  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo superBlockSuite = {
  .name                     = "Super Block (SuperBlock_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultVDOTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = superBlockTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &superBlockSuite;
}
