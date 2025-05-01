/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "encodings.h"

#include "vdoAsserts.h"

enum {
  DATA_SIZE = 10,
};

static struct header HEADER = {
  .id      = 3,
  .version = {1, 3},
  .size    = DATA_SIZE,
};

static u8 DATA[] = {
  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10,
};

/**
 * Check that two headers are the same.
 *
 * @param headerA The first header
 * @param headerB The second header
 **/
static void assertSameHeader(struct header *headerA, struct header *headerB)
{
  CU_ASSERT_EQUAL(headerA->id, headerB->id);
  CU_ASSERT_TRUE(vdo_are_same_version(headerA->version, headerB->version));
  CU_ASSERT_EQUAL(headerA->size, headerB->size);
}

/**
 * Test encode and decoding of headers.
 **/
static void testHeaderCoding(void)
{
  u8 buffer[VDO_ENCODED_HEADER_SIZE];
  size_t offset = 0;
  vdo_encode_header(buffer, &offset, &HEADER);

  struct header header;
  offset = 0;
  vdo_decode_header(buffer, &offset, &header);
  assertSameHeader(&HEADER, &header);

  header.version.minor_version++;
  CU_ASSERT_FALSE(vdo_are_same_version(HEADER.version, header.version));
  header.version.minor_version--;

  header.version.major_version++;
  CU_ASSERT_FALSE(vdo_are_same_version(HEADER.version, header.version));
}

/**
 * Test encode and decode of structured data.
 **/
static void testDataCoding(void)
{
  u8 buffer[VDO_ENCODED_HEADER_SIZE + DATA_SIZE];
  size_t offset = 0;
  vdo_encode_header(buffer, &offset, &HEADER);
  memcpy(buffer + offset, DATA, HEADER.size);

  struct header header;
  offset = 0;
  vdo_decode_header(buffer, &offset, &header);

  u8 data[DATA_SIZE];
  memcpy(data, buffer + offset, header.size);

  assertSameHeader(&HEADER, &header);
  UDS_ASSERT_EQUAL_BYTES(DATA, data, DATA_SIZE);
}

/**********************************************************************/

static CU_TestInfo headerTests[] = {
  { "header coding and version mismatch", testHeaderCoding, },
  { "data coding",                        testDataCoding,   },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo headerSuite = {
  .name                     = "header and structured data (Header_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = headerTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &headerSuite;
}
