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

#include "header.h"
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
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(VDO_ENCODED_HEADER_SIZE, &buffer));
  VDO_ASSERT_SUCCESS(vdo_encode_header(&HEADER, buffer));

  struct header header;
  VDO_ASSERT_SUCCESS(vdo_decode_header(buffer, &header));
  free_buffer(UDS_FORGET(buffer));

  assertSameHeader(&HEADER, &header);

  header.version.minor_version++;
  CU_ASSERT_TRUE(vdo_is_upgradable_version(header.version, HEADER.version));
  CU_ASSERT_FALSE(vdo_is_upgradable_version(HEADER.version, header.version));
  CU_ASSERT_FALSE(vdo_are_same_version(HEADER.version, header.version));
  header.version.minor_version--;

  header.version.major_version++;
  CU_ASSERT_FALSE(vdo_is_upgradable_version(header.version, HEADER.version));
  CU_ASSERT_FALSE(vdo_is_upgradable_version(HEADER.version, header.version));
  CU_ASSERT_FALSE(vdo_are_same_version(HEADER.version, header.version));
}

/**
 * Test encoding and decoding a header with a buffer that is too short.
 **/
static void testHeaderCodingTooShort(void)
{
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(VDO_ENCODED_HEADER_SIZE - 1, &buffer));
  CU_ASSERT_EQUAL(UDS_BUFFER_ERROR, vdo_encode_header(&HEADER, buffer));

  VDO_ASSERT_SUCCESS(put_bytes(buffer, VDO_ENCODED_HEADER_SIZE - 1, &HEADER));

  struct header header;
  CU_ASSERT_EQUAL(UDS_BUFFER_ERROR, vdo_decode_header(buffer, &header));
  free_buffer(UDS_FORGET(buffer));
}

/**
 * Test encode and decode of structured data.
 **/
static void testDataCoding(void)
{
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(VDO_ENCODED_HEADER_SIZE + DATA_SIZE,
                                 &buffer));

  VDO_ASSERT_SUCCESS(vdo_encode_header(&HEADER, buffer));
  VDO_ASSERT_SUCCESS(put_bytes(buffer, HEADER.size, DATA));

  struct header header;
  VDO_ASSERT_SUCCESS(vdo_decode_header(buffer, &header));

  u8 data[DATA_SIZE];
  VDO_ASSERT_SUCCESS(get_bytes_from_buffer(buffer, header.size, data));
  free_buffer(UDS_FORGET(buffer));

  assertSameHeader(&HEADER, &header);
  UDS_ASSERT_EQUAL_BYTES(DATA, data, DATA_SIZE);
}

/**
 * Test encode and decode of structured data with a buffer that is too short.
 **/
static void testDataCodingTooShort(void)
{
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(VDO_ENCODED_HEADER_SIZE + DATA_SIZE - 1,
                                 &buffer));

  VDO_ASSERT_SUCCESS(vdo_encode_header(&HEADER, buffer));
  VDO_ASSERT_SUCCESS(put_bytes(buffer, DATA_SIZE - 1, DATA));

  struct header header;
  VDO_ASSERT_SUCCESS(vdo_decode_header(buffer, &header));

  u8 data[DATA_SIZE];
  CU_ASSERT_EQUAL(UDS_BUFFER_ERROR,
                  get_bytes_from_buffer(buffer, header.size, data));
  CU_ASSERT_EQUAL(DATA_SIZE - 1, content_length(buffer));

  free_buffer(UDS_FORGET(buffer));
}

/**********************************************************************/

static CU_TestInfo headerTests[] = {
  { "header coding and version mismatch", testHeaderCoding,           },
  { "header coding too short",            testHeaderCodingTooShort,   },
  { "data coding",                        testDataCoding,             },
  { "data coding too short",              testDataCodingTooShort,     },
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
