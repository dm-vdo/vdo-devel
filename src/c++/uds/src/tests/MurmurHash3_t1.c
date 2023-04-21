// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "murmurhash3.h"
#include "testPrototypes.h"

static const char *input1 = "The quick brown fox jumps over the lazy dog";
static const char *input2 = "The quick brown fox jumps over the lazy cog";

/**********************************************************************/
static void checkHash(const char *input, const unsigned char expected[])
{
  unsigned char hash[128 / 8];
  // Hash with the seed = 0.
  murmurhash3_128(input, (int) strlen(input), 0, hash);
  UDS_ASSERT_EQUAL_BYTES(expected, hash, sizeof(hash));
}

/**********************************************************************/
static void testHash128(void)
{
  const unsigned char result1[]
    = { 0x6c, 0x1b, 0x07, 0xbc, 0x7b, 0xbc, 0x4b, 0xe3,
        0x47, 0x93, 0x9a, 0xc4, 0xa9, 0x3c, 0x43, 0x7a, };
  checkHash(input1, result1);

  const unsigned char result2[]
    = { 0x9a, 0x26, 0x85, 0xff, 0x70, 0xa9, 0x8c, 0x65,
        0x3e, 0x5c, 0x8e, 0xa6, 0xea, 0xe3, 0xfe, 0x43, };
  checkHash(input2, result2);
}

/**********************************************************************/
static void checkRecordName(const char *input, struct uds_record_name expected)
{
  struct uds_record_name recordName;
  // Hash with the seed used by VDO.
  murmurhash3_128(input, strlen(input), 0x62ea60be, &recordName.name);
  UDS_ASSERT_BLOCKNAME_EQUAL(expected.name, recordName.name);
  // Make sure hash_record_name produces the same result.
  recordName = hash_record_name(input, strlen(input));
  UDS_ASSERT_BLOCKNAME_EQUAL(expected.name, recordName.name);
}

/**********************************************************************/
static void testHashRecordName(void)
{
  struct uds_record_name result1 = {
    {
      0x43, 0x79, 0x6d, 0x74, 0xe3, 0x93, 0x86, 0x45,
      0xc3, 0x89, 0x39, 0x7e, 0x23, 0xfc, 0xfd, 0x54,
    }
  };
  checkRecordName(input1, result1);

  struct uds_record_name result2 = {
    {
      0x2d, 0x32, 0x3c, 0x15, 0x21, 0x6c, 0x39, 0xfb,
      0x36, 0x79, 0xfc, 0x8d, 0x07, 0x3c, 0xcd, 0xa6,
    }
  };
  checkRecordName(input2, result2);
}

/**********************************************************************/
static CU_TestInfo murmurTests[] = {
  {"murmurhash3_128",      testHash128 },
  {"murmurHashRecordName", testHashRecordName },
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
