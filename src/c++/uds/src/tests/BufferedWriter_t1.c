// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "buffered-writer.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static const char BOSTON[] =
  "I come from the city of Boston,\n"
  "The home of the bean and the cod,\n"
  "Where Cabots speak only to Lowells,\n"
  "And Lowells speak only to God.\n";

/**********************************************************************/
static void bufferTest(void)
{
  enum {
    BOSTON_LEN  = sizeof(BOSTON) - 1,
    REGION_SIZE = 8 * UDS_BLOCK_SIZE,
    BEST_SIZE   = UDS_BLOCK_SIZE,
  };
  struct io_factory *factory = getTestIOFactory();

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, 0, REGION_SIZE,
                                              &writer));

  // write until the buffered writer flushes by itself
  unsigned int count = 0;
  size_t remaining = space_remaining_in_write_buffer(writer);
  CU_ASSERT(remaining == BEST_SIZE);
  for (;;) {
    UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
    ++count;
    size_t tmp = space_remaining_in_write_buffer(writer);
    if (remaining < tmp) {
      break;
    }
    remaining = tmp;
  }
  UDS_ASSERT_SUCCESS(flush_buffered_writer(writer));
  free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, REGION_SIZE,
                                              &reader));
  byte inputArray[BOSTON_LEN];
  unsigned int i;
  for (i = 0; i < count; ++i) {
    memset(inputArray, i, BOSTON_LEN);
    UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, inputArray, BOSTON_LEN));
    UDS_ASSERT_EQUAL_BYTES(inputArray, BOSTON, BOSTON_LEN);
  }
  free_buffered_reader(reader);
  put_uds_io_factory(factory);
}

/**********************************************************************/
static void largeWriteTest(void)
{
  enum { REGION_SIZE = 32 * UDS_BLOCK_SIZE };
  struct io_factory *factory = getTestIOFactory();

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, 0, REGION_SIZE,
                                              &writer));

  size_t bufSize = space_remaining_in_write_buffer(writer);
  size_t buflen = 4 * bufSize;
  byte *bigbuf, *verbuf;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(buflen, byte, __func__, &bigbuf));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(buflen, byte, __func__, &verbuf));

  fillBufferFromSeed(0, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, bigbuf, buflen));
  CU_ASSERT_EQUAL(bufSize, space_remaining_in_write_buffer(writer));

  size_t buflen1 = bufSize / 3;
  fillBufferFromSeed(1, bigbuf, buflen1);
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, bigbuf, buflen1));
  CU_ASSERT_EQUAL(bufSize - buflen1, space_remaining_in_write_buffer(writer));

  fillBufferFromSeed(2, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, bigbuf, buflen));
  CU_ASSERT_EQUAL(bufSize - buflen1, space_remaining_in_write_buffer(writer));

  size_t buflen3 = 7 * bufSize / 8;
  fillBufferFromSeed(3, bigbuf, buflen3);
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, bigbuf, buflen3));
  CU_ASSERT_EQUAL(bufSize - 5 * bufSize / 24,
                  space_remaining_in_write_buffer(writer));

  UDS_ASSERT_SUCCESS(flush_buffered_writer(writer));
  CU_ASSERT_EQUAL(bufSize, space_remaining_in_write_buffer(writer));
  free_buffered_writer(writer);

  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, REGION_SIZE,
                                              &reader));

  fillBufferFromSeed(0, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, verbuf, buflen));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen);

  fillBufferFromSeed(1, bigbuf, buflen1);
  UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, verbuf, buflen1));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen1);

  fillBufferFromSeed(2, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, verbuf, buflen));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen);

  fillBufferFromSeed(3, bigbuf, buflen3);
  UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, verbuf, buflen3));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen3);

  free_buffered_reader(reader);
  put_uds_io_factory(factory);
  UDS_FREE(bigbuf);
  UDS_FREE(verbuf);
}

/**********************************************************************/
static void zeroTest(void)
{
  enum {
    BOSTON_LEN = sizeof(BOSTON) - 1,
    ZERO_LEN   = 13,
  };
  byte zeros[ZERO_LEN];
  memset(zeros, 0, ZERO_LEN);
  struct io_factory *factory = getTestIOFactory();

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, 0, 4 * UDS_BLOCK_SIZE,
                                              &writer));
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(write_zeros_to_buffered_writer(writer, ZERO_LEN));
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(flush_buffered_writer(writer));
  free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, 4 * UDS_BLOCK_SIZE,
                                              &reader));
  UDS_ASSERT_SUCCESS(verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(verify_buffered_data(reader, zeros,  ZERO_LEN));
  UDS_ASSERT_SUCCESS(verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  free_buffered_reader(reader);
  put_uds_io_factory(factory);
}

/**********************************************************************/
static void verifyTest(void)
{
  int i;
  static const char X1[] = "xxxxxx1";
  static const char X2[] = "xxxxxx2";
  enum { BOSTON_LEN = sizeof(BOSTON) - 1 };
  enum { X1_LEN = sizeof(X1) - 1 };
  enum { X2_LEN = sizeof(X2) - 1 };
  enum { COUNT = UDS_BLOCK_SIZE / X1_LEN };
  struct io_factory *factory = getTestIOFactory();

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(open_uds_buffered_writer(factory, 0, 4 * UDS_BLOCK_SIZE,
                                              &writer));
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  for (i = 0; i < COUNT; i++) {
    UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, X1, X1_LEN));
    UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, X2, X2_LEN));
  }
  UDS_ASSERT_SUCCESS(write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(flush_buffered_writer(writer));
  free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, 4 * UDS_BLOCK_SIZE,
                                              &reader));
  UDS_ASSERT_SUCCESS(verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  for (i = 0; i < COUNT; i++) {
    UDS_ASSERT_ERROR(UDS_CORRUPT_DATA, verify_buffered_data(reader, X2, X2_LEN));
    UDS_ASSERT_SUCCESS(verify_buffered_data(reader, X1, X1_LEN));
    UDS_ASSERT_ERROR(UDS_CORRUPT_DATA, verify_buffered_data(reader, X1, X1_LEN));
    UDS_ASSERT_SUCCESS(verify_buffered_data(reader, X2, X2_LEN));
  }
  UDS_ASSERT_SUCCESS(verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  free_buffered_reader(reader);
  put_uds_io_factory(factory);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "buffered writer and reader", bufferTest     },
  { "large writes",               largeWriteTest },
  { "zero writes",                zeroTest       },
  { "verify errors",              verifyTest     },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "BufferedWriter_t1",
  .tests = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
