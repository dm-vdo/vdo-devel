// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/murmurhash3.h>

#include "albtest.h"
#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
static void checkHash(const char *input, const uint8_t expected[])
{
  uint8_t hash[128 / 8];
  murmurhash3_128(input, (int) strlen(input), 0, hash);
  UDS_ASSERT_EQUAL_BYTES(expected, hash, sizeof(hash));
}

/**********************************************************************/
static void testHash128(void)
{
  const char *input1 = "The quick brown fox jumps over the lazy dog";
  const uint8_t result1[] = {
    0x6c, 0x1b, 0x07, 0xbc, 0x7b, 0xbc, 0x4b, 0xe3,
    0x47, 0x93, 0x9a, 0xc4, 0xa9, 0x3c, 0x43, 0x7a,
  };
  checkHash(input1, result1);

  const char *input2 = "The quick brown fox jumps over the lazy cog";
  const uint8_t result2[] = {
    0x9a, 0x26, 0x85, 0xff, 0x70, 0xa9, 0x8c, 0x65,
    0x3e, 0x5c, 0x8e, 0xa6, 0xea, 0xe3, 0xfe, 0x43,
  };
  checkHash(input2, result2);
}

/**********************************************************************/
static void checkChunkName(const char *input, struct uds_record_name expected)
{
  struct uds_record_name name = murmurHashChunkName(input, strlen(input), 0);
  UDS_ASSERT_BLOCKNAME_EQUAL(expected.name, name.name);
}

/**********************************************************************/
static void testChunkName(void)
{
  const char *input1 = "The quick brown fox jumps over the lazy dog";
  static const unsigned char hash1[] = {
    0x43, 0x79, 0x6d, 0x74, 0xe3, 0x93, 0x86, 0x45,
    0xc3, 0x89, 0x39, 0x7e, 0x23, 0xfc, 0xfd, 0x54,
    0xf2, 0x0a, 0xd3, 0x4d, 0x84, 0x5b, 0x70, 0x82,
    0x8f, 0x91, 0x81, 0x45, 0xa0, 0xb6, 0x6d, 0xda,
  };
  struct uds_record_name result;
  memcpy(result.name, hash1, UDS_RECORD_NAME_SIZE);
  checkChunkName(input1, result);

  const char *input2 = "The quick brown fox jumps over the lazy cog";
  static const unsigned char hash2[] = {
      0x2d, 0x32, 0x3c, 0x15, 0x21, 0x6c, 0x39, 0xfb,
      0x36, 0x79, 0xfc, 0x8d, 0x07, 0x3c, 0xcd, 0xa6,
      0xc9, 0xc9, 0x8a, 0x6f, 0xac, 0xeb, 0x78, 0x82,
      0xac, 0x86, 0xaa, 0x52, 0x99, 0x0b, 0x9c, 0x19,
  };
  memcpy(result.name, hash2, UDS_RECORD_NAME_SIZE);
  checkChunkName(input2, result);
}

/**********************************************************************/

static const CU_TestInfo murmurTests[] = {
  {"MurmurHash3_x64_128", testHash128 },
  {"murmurHashChunkName", testChunkName },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo murmurSuite = {
  .name                     = "MurmurHash3_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = murmurTests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &murmurSuite;
}
