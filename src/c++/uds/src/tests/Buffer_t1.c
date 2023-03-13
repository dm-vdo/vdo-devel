// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "buffer.h"

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"

enum { SIZE = 28 };

static const bool      BOOL1     = true;
static const bool      BOOL2     = false;
static const u8       *BYTES     = (u8 *) "ABCD";
static const uint16_t  UINT16T   = 27;
static const uint32_t  UINT32T   = 7546;
static const uint64_t  NUMBERS[] = { 0x0102030405060708, 0x1122334455667788 };

/**
 * Fill a buffer one byte at a time with sequentially increasing byte values,
 * and check that the filling was successful.
 *
 * @param buffer       The buffer to fill
 * @param expectedSize The expected number of bytes to be filled
 **/
static void fillBufferWithBytes(struct buffer *buffer, size_t expectedSize)
{
  unsigned int i;
  for (i = 0; i < expectedSize; i++) {
    CU_ASSERT_EQUAL(uds_available_space(buffer), expectedSize - i);
    UDS_ASSERT_SUCCESS(uds_put_byte(buffer, (u8) i));
  }

  // Check that the buffer is full
  UDS_ASSERT_ERROR(UDS_BUFFER_ERROR,
                   uds_put_byte(buffer, (u8) (expectedSize + 1)));
  CU_ASSERT_EQUAL(uds_available_space(buffer), 0);
  CU_ASSERT_FALSE(uds_ensure_available_space(buffer, 1));
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(uds_content_length(buffer), uds_buffer_length(buffer));

  // Check that the contents are as expected
  for (i = 0; i < expectedSize; i++) {
    u8 b;
    UDS_ASSERT_SUCCESS(uds_get_byte(buffer, &b));
    CU_ASSERT_EQUAL(b, (u8) i);
  }
  // Reset the start of buffer
  UDS_ASSERT_SUCCESS(uds_rewind_buffer(buffer, uds_buffer_length(buffer)));
}

/**
 * Extract sequentially increasing bytes from a buffer one byte at a time.
 *
 * @param buffer       The buffer to extract from
 * @param expectedSize The expected number of bytes to be extracted
 * @param startByte    The expected value of the first byte to be extracted
 **/
static void extractBytesFromBuffer(struct buffer *buffer,
                                   size_t  expectedSize,
                                   u8      startByte)
{
  unsigned int i;
  for (i = 0; i < expectedSize; i++) {
    u8 b;
    CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize - i);
    UDS_ASSERT_SUCCESS(uds_get_byte(buffer, &b));
    CU_ASSERT_EQUAL(b, (u8) i + startByte);
  }
}

/**
 * Check that a buffer has no content and compacts correctly.
 *
 * @param buffer       The buffer to check
 * @param expectedSize The expected number of bytes available after compaction
 **/
static void compactEmptyBuffer(struct buffer *buffer, size_t expectedSize)
{
  CU_ASSERT_EQUAL(uds_content_length(buffer), 0);
  CU_ASSERT_TRUE(uds_ensure_available_space(buffer, expectedSize));
  CU_ASSERT_FALSE(uds_ensure_available_space(buffer, expectedSize + 1));
  CU_ASSERT_EQUAL(uds_content_length(buffer), 0);
}

/**
 * Test basic buffer operations.
 **/
static void testBasicBuffer(void)
{
  // Make a new buffer and check that it has the correct amount of space.
  struct buffer *buffer;
  UDS_ASSERT_SUCCESS(make_uds_buffer(SIZE, &buffer));
  CU_ASSERT_EQUAL(uds_available_space(buffer), SIZE);
  size_t s;
  for (s = 0; s <= SIZE + 10; s++) {
    CU_ASSERT_EQUAL((s <= SIZE), uds_ensure_available_space(buffer, s));
  }

  // Fill the buffer one byte at a time
  fillBufferWithBytes(buffer, SIZE);

  // Unfill the buffer one byte at a time.
  extractBytesFromBuffer(buffer, SIZE, 0);

  // Check that we've emptied the buffer and can compact it
  compactEmptyBuffer(buffer, SIZE);

  // Fill it again
  fillBufferWithBytes(buffer, SIZE);

  // Skip the first half of the buffer
  UDS_ASSERT_ERROR(UDS_BUFFER_ERROR, uds_skip_forward(buffer, SIZE + 1));
  UDS_ASSERT_SUCCESS(uds_skip_forward(buffer, SIZE / 2));
  extractBytesFromBuffer(buffer, SIZE / 2, SIZE / 2);

  // Check that we've emptied the buffer and can compact it
  compactEmptyBuffer(buffer, SIZE);

  free_uds_buffer(UDS_FORGET(buffer));
}

/**
 * Check the contents of a buffer that was filled with assorted data.
 **/
static void checkContents(struct buffer *buffer)
{
  size_t expectedSize = SIZE;
  CU_ASSERT_EQUAL(SIZE, uds_content_length(buffer));

  bool b;
  UDS_ASSERT_SUCCESS(uds_get_boolean(buffer, &b));
  expectedSize--;
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(b, BOOL1);

  UDS_ASSERT_SUCCESS(uds_get_boolean(buffer, &b));
  expectedSize--;
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(b, BOOL2);

  u8 bytes[4];
  UDS_ASSERT_SUCCESS(uds_get_bytes_from_buffer(buffer, 4, bytes));
  expectedSize -= 4;
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  UDS_ASSERT_EQUAL_BYTES(bytes, BYTES, 4);

  uint16_t uint16t;
  UDS_ASSERT_SUCCESS(uds_get_u16_le_from_buffer(buffer, &uint16t));
  expectedSize -= 2;
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(uint16t, UINT16T);

  uint32_t uint32t;
  UDS_ASSERT_SUCCESS(uds_get_u32_le_from_buffer(buffer, &uint32t));
  expectedSize -= 4;
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(uint32t, UINT32T);

  uint64_t numbers[2];
  UDS_ASSERT_SUCCESS(uds_get_u64_les_from_buffer(buffer, 2, numbers));
  expectedSize -= (8 * 2);
  CU_ASSERT_EQUAL(uds_content_length(buffer), expectedSize);
  CU_ASSERT_EQUAL(numbers[0], NUMBERS[0]);
  CU_ASSERT_EQUAL(numbers[1], NUMBERS[1]);
}

/**
 * Test filling and extracting of a buffer with different types of data.
 **/
static void testBufferDataTypes(void)
{
  struct buffer *buffer;
  UDS_ASSERT_SUCCESS(make_uds_buffer(SIZE, &buffer));

  // Fill the buffer with assorted data
  UDS_ASSERT_SUCCESS(uds_put_boolean(buffer, BOOL1));
  CU_ASSERT_EQUAL(1, uds_content_length(buffer));
  UDS_ASSERT_SUCCESS(uds_put_boolean(buffer, BOOL2));
  CU_ASSERT_EQUAL(2, uds_content_length(buffer));

  UDS_ASSERT_SUCCESS(uds_put_bytes(buffer, 4, BYTES));
  CU_ASSERT_EQUAL(6, uds_content_length(buffer));

  UDS_ASSERT_SUCCESS(uds_put_u16_le_into_buffer(buffer, UINT16T));
  CU_ASSERT_EQUAL(8, uds_content_length(buffer));

  UDS_ASSERT_SUCCESS(uds_put_u32_le_into_buffer(buffer, UINT32T));
  CU_ASSERT_EQUAL(12, uds_content_length(buffer));

  UDS_ASSERT_SUCCESS(uds_put_u64_les_into_buffer(buffer, 2, NUMBERS));
  CU_ASSERT_EQUAL(28, uds_content_length(buffer));

  CU_ASSERT_EQUAL(0, uds_available_space(buffer));
  CU_ASSERT_FALSE(uds_ensure_available_space(buffer, 1));

  // Copy the contents
  u8 copy[SIZE];
  memcpy(copy, uds_get_buffer_contents(buffer), SIZE);

  checkContents(buffer);
  free_uds_buffer(UDS_FORGET(buffer));

  UDS_ASSERT_SUCCESS(uds_wrap_buffer(copy, SIZE, SIZE, &buffer));
  checkContents(buffer);
  free_uds_buffer(UDS_FORGET(buffer));
}

/**********************************************************************/
static void testZeroBytes(void)
{
  struct buffer *buffer;
  UDS_ASSERT_SUCCESS(make_uds_buffer(SIZE, &buffer));
  fillBufferWithBytes(buffer, SIZE);
  UDS_ASSERT_SUCCESS(uds_skip_forward(buffer, SIZE));
  uds_compact_buffer(buffer);
  UDS_ASSERT_SUCCESS(uds_zero_bytes(buffer, SIZE / 2));
  CU_ASSERT_EQUAL(uds_content_length(buffer), SIZE / 2);
  CU_ASSERT_EQUAL(uds_available_space(buffer), SIZE / 2);

  unsigned int i;
  for (i = 0; i < SIZE / 2; i++) {
    u8 b;
    UDS_ASSERT_SUCCESS(uds_get_byte(buffer, &b));
    CU_ASSERT_EQUAL(b, 0);
  }

  free_uds_buffer(UDS_FORGET(buffer));
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "basic functionality",                     testBasicBuffer },
  { "filling/extracting different data types", testBufferDataTypes },
  { "zeroing of contents",                     testZeroBytes },
  CU_TEST_INFO_NULL,
};

/**********************************************************************/
static const CU_SuiteInfo suite = {
  .name                     = "Buffer_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
