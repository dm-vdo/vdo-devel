/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "constants.h"
#include "compressed-block.h"

#include "vdoAsserts.h"

enum {
  INVALID_VERSION = -1,
};

struct compressed_block compressedBlock;

/**********************************************************************/
static void initialize(void)
{
  memset(&compressedBlock, 0, sizeof(struct compressed_block));
}

/**********************************************************************/
static void testEmptyBlock(void)
{
  for (enum block_mapping_state i = VDO_MAPPING_STATE_UNMAPPED;
       i < VDO_MAPPING_STATE_COMPRESSED_BASE; i++) {
    uint16_t fragmentOffset, fragmentSize;
    CU_ASSERT_EQUAL(VDO_INVALID_FRAGMENT,
                    vdo_get_compressed_block_fragment(i,
                                                      &compressedBlock,
                                                      &fragmentOffset,
                                                      &fragmentSize));
  }
}

/**********************************************************************/
static void testInvalidBlock(void)
{
  compressedBlock.header.version.major_version
    = __cpu_to_le32(INVALID_VERSION);

  for (unsigned int i = 0; i < VDO_MAX_COMPRESSION_SLOTS; ++i) {
    uint16_t fragmentOffset, fragmentSize;
    CU_ASSERT_EQUAL(VDO_INVALID_FRAGMENT,
                    vdo_get_compressed_block_fragment(vdo_get_state_for_slot(i),
                                                      &compressedBlock,
                                                      &fragmentOffset,
                                                      &fragmentSize));
  }
}

/**********************************************************************/
static void testAbsurdBlock(void)
{
  vdo_initialize_compressed_block(&compressedBlock, 101);
  for (unsigned int i = 1; i < VDO_MAX_COMPRESSION_SLOTS; ++i) {
    compressedBlock.header.sizes[i] = __cpu_to_le16(VDO_BLOCK_SIZE + i * 101);
  }

  uint16_t fragmentOffset, fragmentSize;
  CU_ASSERT_EQUAL(VDO_SUCCESS,
                  vdo_get_compressed_block_fragment(vdo_get_state_for_slot(0),
                                                    &compressedBlock,
                                                    &fragmentOffset,
                                                    &fragmentSize));

  for (unsigned int i = 1; i < VDO_MAX_COMPRESSION_SLOTS; ++i) {
    CU_ASSERT_EQUAL(VDO_INVALID_FRAGMENT,
                    vdo_get_compressed_block_fragment(vdo_get_state_for_slot(i),
                                                      &compressedBlock,
                                                      &fragmentOffset,
                                                      &fragmentSize));
  }
}

/**********************************************************************/
static void testValidFragments(void)
{
  char originalData[VDO_BLOCK_SIZE];

  int j = ' ';
  for (unsigned int i = 0; i < sizeof(originalData); ++i, ++j) {
    if (j > '~') {
      j = ' ';
    }
    originalData[i] = (char) j;
  }

  unsigned int offsets[VDO_MAX_COMPRESSION_SLOTS + 1] = {
       0,
       200,  400,  440,  960, 1130, 1131, 1131,
       1290, 2055, 3012, 3994, 3994, 4050,
       (VDO_BLOCK_SIZE - sizeof(struct compressed_block_header))
  };

  for (unsigned int i = 0; i < VDO_MAX_COMPRESSION_SLOTS; ++i) {
    if (i == 0) {
      /* The compressor will put the fragment 0 data in place already */
      memcpy(compressedBlock.data, originalData, offsets[1]);
      vdo_initialize_compressed_block(&compressedBlock, offsets[1]);
      continue;
    }

    vdo_put_compressed_block_fragment(&compressedBlock,
                                      i,
                                      offsets[i],
                                      originalData + offsets[i],
                                      offsets[i + 1] - offsets[i]);
  }

  for (unsigned int i = 0; i < VDO_MAX_COMPRESSION_SLOTS; ++i) {
    uint16_t fragmentOffset, fragmentSize;
    CU_ASSERT_EQUAL(VDO_SUCCESS,
                    vdo_get_compressed_block_fragment(vdo_get_state_for_slot(i),
                                                      &compressedBlock,
                                                      &fragmentOffset,
                                                      &fragmentSize));
    CU_ASSERT_EQUAL(fragmentOffset, offsets[i]);

    size_t expectedSize = offsets[i + 1] - offsets[i];
    CU_ASSERT_EQUAL(fragmentSize, expectedSize);

    UDS_ASSERT_EQUAL_BYTES(compressedBlock.data + fragmentOffset,
                           originalData + offsets[i],
                           fragmentSize);
  }
}

/**********************************************************************/
static CU_TestInfo compressedBlockTests[] = {
  { "empty block",     testEmptyBlock     },
  { "invalid block",   testInvalidBlock   },
  { "absurd block",    testAbsurdBlock    },
  { "valid fragments", testValidFragments },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo compressedBlockSuite = {
  .name                     = "compressed_block tests (CompressedBlock_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = NULL,
  .tests                    = compressedBlockTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &compressedBlockSuite;
}
