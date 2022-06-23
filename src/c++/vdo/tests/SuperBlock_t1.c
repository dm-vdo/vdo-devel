/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "buffer.h"
#include "memory-alloc.h"
#include "syscalls.h"

#include "release-versions.h"
#include "super-block.h"
#include "super-block-codec.h"
#include "types.h"

#include "asyncLayer.h"
#include "userVDO.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  DATA1_SIZE           = 10,
  DATA2_SIZE           = 20,
};

static const byte DATA1[] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
};

static const byte DATA2[] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
};

/*
 * A captured encoding of the super block version 12.0 wrapping the test
 * payload data above. This is used by testEncoding() to check that the
 * encoding format hasn't changed and is platform-independent.
 */
static byte EXPECTED_SUPERBLOCK_12_0_ENCODING[] =
  {
                                                    // header
    0x00, 0x00, 0x00, 0x00,                         //   .id = VDO_SUPER_BLOCK
    0x0C, 0x00, 0x00, 0x00,                         //   .majorVersion = 12
    0x00, 0x00, 0x00, 0x00,                         //   .minorVersion = 0
    0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   .size = 34
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // payload = DATA1 + DATA2
    0x09, 0x0a, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
    0x55, 0xe2, 0xc7, 0x3c,                         // checksum = 0x3cc7e255
  };

static char                    block[VDO_BLOCK_SIZE];
static struct vdo_super_block *superBlock;
static struct vdo_super_block *loadedSuperBlock;
static UserVDO                *userVDO;
static UserVDO                *loadedVDO;
static size_t                  superBlockSize;

/**
 * Initialize a super block.
 **/
static void initializeSuperBlockT1(void)
{
  superBlockSize = vdo_get_super_block_fixed_size() + DATA1_SIZE + DATA2_SIZE;
  initializeDefaultBasicTest();
}

/**********************************************************************/
static void initializeVDO(UserVDO **vdoPtr)
{
  VDO_ASSERT_SUCCESS(makeUserVDO(layer, vdoPtr));
  UserVDO *userVDO = *vdoPtr;
  userVDO->geometry.regions[VDO_DATA_REGION] = (struct volume_region) {
    .id = VDO_DATA_REGION,
    .start_block = getSuperBlockLocation(),
  };
}

/**
 * Clean up from a test.
 **/
static void tearDownSuperBlockT1(void)
{
  vdo_free_super_block(UDS_FORGET(superBlock));
  freeUserVDO(&userVDO);
  tearDownVDOTest();
}

/**
 * Check that a super block codec contains the correct data
 *
 * @param a  The first codec to validate
 * @param b  The second codec to validate
 **/
static void assertEquivalentCodecs(struct super_block_codec *a,
                                   struct super_block_codec *b)
{
  UDS_ASSERT_EQUAL_BYTES(a->encoded_super_block, b->encoded_super_block,
                         VDO_BLOCK_SIZE);
  CU_ASSERT_TRUE(equal_buffers(a->component_buffer, b->component_buffer));
  CU_ASSERT_TRUE(equal_buffers(a->block_buffer, b->block_buffer));
}

/**********************************************************************/
static struct super_block_codec *encodeSuperBlockPayload(bool async)
{
  struct super_block_codec *codec;
  if (async) {
    VDO_ASSERT_SUCCESS(vdo_make_super_block(vdo, &superBlock));
    codec = vdo_get_super_block_codec(superBlock);
  } else {
    initializeVDO(&userVDO);
    codec = &userVDO->superBlockCodec;
  }

  // Set up what we think the super block should look like for later
  // comparison.
  memset(block, 0, VDO_BLOCK_SIZE);
  memcpy(block, EXPECTED_SUPERBLOCK_12_0_ENCODING,
         sizeof(EXPECTED_SUPERBLOCK_12_0_ENCODING));

  struct buffer *buffer = codec->component_buffer;
  VDO_ASSERT_SUCCESS(reset_buffer_end(buffer, 0));
  VDO_ASSERT_SUCCESS(put_bytes(buffer, DATA1_SIZE, DATA1));
  VDO_ASSERT_SUCCESS(put_bytes(buffer, DATA2_SIZE, DATA2));

  return codec;
}

/**********************************************************************/
static void saveSuperBlockAction(struct vdo_completion *completion)
{
  vdo_save_super_block(superBlock, getSuperBlockLocation(), completion);
}

/**********************************************************************/
static void attemptSaveSuperBlock(bool saveAsync, int expectedResult)
{
  if (saveAsync) {
    performActionExpectResult(saveSuperBlockAction, expectedResult);
  } else {
    CU_ASSERT_EQUAL(expectedResult, saveSuperBlock(userVDO));
  }
}

/**********************************************************************/
static void cleanUpLoad(bool async) {
  if (async) {
    vdo_free_super_block(UDS_FORGET(loadedSuperBlock));
  } else {
    freeUserVDO(&loadedVDO);
  }
}

/**********************************************************************/
static void loadSuperBlockAction(struct vdo_completion *completion)
{
  vdo_load_super_block(vdo,
                       completion,
                       getSuperBlockLocation(),
                       &loadedSuperBlock);
}

/**********************************************************************/
static void attemptLoadSuperBlock(bool loadAsync, int expectedResult)
{
  if (loadAsync) {
    performActionExpectResult(loadSuperBlockAction, expectedResult);
  } else {
    initializeVDO(&loadedVDO);
    CU_ASSERT_EQUAL(expectedResult, loadSuperBlock(loadedVDO));
  }
}

/**********************************************************************/
static struct super_block_codec *attemptSuccessfulLoad(bool async)
{
  attemptLoadSuperBlock(async, VDO_SUCCESS);
  if (async) {
    return vdo_get_super_block_codec(loadedSuperBlock);
  } else {
    return &loadedVDO->superBlockCodec;
  }
}

/**********************************************************************/
static void attemptFailedLoad(bool async, int expectedResult)
{
  attemptLoadSuperBlock(async, expectedResult);
  cleanUpLoad(async);
}

/**********************************************************************/
static void superBlockLoad(struct super_block_codec *saved_codec, bool async)
{
  assertEquivalentCodecs(saved_codec, attemptSuccessfulLoad(async));
  cleanUpLoad(async);
}

/**
 * Test saving and loading of a super block.
 **/
static void doSaveAndLoadTest(bool saveAsync)
{
  /* Do a normal save and load */
  struct super_block_codec *codec = encodeSuperBlockPayload(saveAsync);
  attemptSaveSuperBlock(saveAsync, VDO_SUCCESS);
  superBlockLoad(codec, false);
  superBlockLoad(codec, true);

  /* Break the checksum */
  memset(block + superBlockSize - sizeof(uint32_t), 0, sizeof(uint32_t));
  VDO_ASSERT_SUCCESS(layer->writer(layer, getSuperBlockLocation(), 1, block));
  attemptFailedLoad(false, VDO_CHECKSUM_MISMATCH);
  attemptFailedLoad(true, VDO_CHECKSUM_MISMATCH);
}

/**
 * Test saving and loading of a super block.
 **/
static void testSyncSaveAndLoad(void)
{
  doSaveAndLoadTest(false);
}

/**
 * Test saving and loading of a super block.
 **/
static void testAsyncSaveAndLoad(void)
{
  doSaveAndLoadTest(true);
}

/**
 * Test that a super_block is correctly decoded and re-encoded regardless of
 * the endianness of the test platform.
 **/
static void testEncoding(void)
{
  // Fill in the expected DATA1+DATA2 payload for comparison.
  struct super_block_codec *codec = encodeSuperBlockPayload(false);
  VDO_ASSERT_SUCCESS(vdo_encode_super_block(codec));

  // Test decoding by stashing the test encoding in the layer and loading it.
  physical_block_number_t superBlockLocation = getSuperBlockLocation();
  VDO_ASSERT_SUCCESS(layer->writer(layer, superBlockLocation, 1, block));

  initializeVDO(&loadedVDO);
  VDO_ASSERT_SUCCESS(loadSuperBlock(loadedVDO));

  // Verify the contents of the loaded codec
  assertEquivalentCodecs(codec, &loadedVDO->superBlockCodec);

  // Re-encode it by saving it back out to the layer.
  VDO_ASSERT_SUCCESS(saveSuperBlock(loadedVDO));

  // Verify the encoding by reading it from the layer and comparing it.
  VDO_ASSERT_SUCCESS(layer->reader(layer, superBlockLocation, 1, block));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_SUPERBLOCK_12_0_ENCODING,
                         block, sizeof(EXPECTED_SUPERBLOCK_12_0_ENCODING));
}

/**********************************************************************/

static CU_TestInfo superBlockTests[] = {
  { "test save and load",       testSyncSaveAndLoad  },
  { "test save and load async", testAsyncSaveAndLoad },
  { "test encoding",            testEncoding         },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo superBlockSuite = {
  .name                     = "Super Block (SuperBlock_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeSuperBlockT1,
  .cleaner                  = tearDownSuperBlockT1,
  .tests                    = superBlockTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &superBlockSuite;
}
