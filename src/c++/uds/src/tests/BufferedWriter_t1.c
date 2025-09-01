// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static const u8 BOSTON[] =
  "I come from the city of Boston,\n"
  "The home of the bean and the cod,\n"
  "Where Cabots speak only to Lowells,\n"
  "And Lowells speak only to God.\n";

enum {
  BOSTON_LEN    = sizeof(BOSTON) - 1,
  REGION_BLOCKS = 12,
  ZERO_LEN      = 13,
};

/**********************************************************************/
static void bufferTest(void)
{
  struct io_factory *factory;
  struct block_device *testDevice = getTestBlockDevice();
  UDS_ASSERT_SUCCESS(uds_make_io_factory(testDevice, &factory));

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(uds_make_buffered_writer(factory, 0, REGION_BLOCKS, &writer));

  // write until the buffered writer flushes by itself
  unsigned int count = 0;
  size_t written = 0;
  for (;;) {
    UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
    ++count;
    written += BOSTON_LEN;
    if (written > UDS_BLOCK_SIZE) {
      break;
    }
  }
  UDS_ASSERT_SUCCESS(uds_flush_buffered_writer(writer));
  uds_free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, 0, REGION_BLOCKS, &reader));
  u8 inputArray[BOSTON_LEN];
  unsigned int i;
  for (i = 0; i < count; ++i) {
    memset(inputArray, i, BOSTON_LEN);
    UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, inputArray, BOSTON_LEN));
    UDS_ASSERT_EQUAL_BYTES(inputArray, BOSTON, BOSTON_LEN);
  }
  uds_free_buffered_reader(reader);
  uds_put_io_factory(factory);
  putTestBlockDevice(testDevice);
}

/**********************************************************************/
static void largeWriteTest(void)
{
  struct io_factory *factory;
  struct block_device *testDevice = getTestBlockDevice();
  UDS_ASSERT_SUCCESS(uds_make_io_factory(testDevice, &factory));

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(uds_make_buffered_writer(factory, 0, REGION_BLOCKS, &writer));

  size_t bufSize = UDS_BLOCK_SIZE;
  size_t buflen = 4 * bufSize;
  u8 *bigbuf, *verbuf;
  UDS_ASSERT_SUCCESS(vdo_allocate(buflen, __func__, &bigbuf));
  UDS_ASSERT_SUCCESS(vdo_allocate(buflen, __func__, &verbuf));

  fillBufferFromSeed(0, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, bigbuf, buflen));

  size_t buflen1 = bufSize / 3;
  fillBufferFromSeed(1, bigbuf, buflen1);
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, bigbuf, buflen1));

  fillBufferFromSeed(2, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, bigbuf, buflen));

  size_t buflen3 = 7 * bufSize / 8;
  fillBufferFromSeed(3, bigbuf, buflen3);
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, bigbuf, buflen3));

  UDS_ASSERT_SUCCESS(uds_flush_buffered_writer(writer));
  uds_free_buffered_writer(writer);

  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, 0, REGION_BLOCKS, &reader));

  fillBufferFromSeed(0, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, verbuf, buflen));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen);

  fillBufferFromSeed(1, bigbuf, buflen1);
  UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, verbuf, buflen1));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen1);

  fillBufferFromSeed(2, bigbuf, buflen);
  UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, verbuf, buflen));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen);

  fillBufferFromSeed(3, bigbuf, buflen3);
  UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, verbuf, buflen3));
  UDS_ASSERT_EQUAL_BYTES(verbuf, bigbuf, buflen3);

  uds_free_buffered_reader(reader);
  uds_put_io_factory(factory);
  putTestBlockDevice(testDevice);
  vdo_free(bigbuf);
  vdo_free(verbuf);
}

/**********************************************************************/
static void zeroTest(void)
{
  u8 zeros[ZERO_LEN];
  memset(zeros, 0, ZERO_LEN);
  struct io_factory *factory;
  struct block_device *testDevice = getTestBlockDevice();
  UDS_ASSERT_SUCCESS(uds_make_io_factory(testDevice, &factory));

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(uds_make_buffered_writer(factory, 0, 4, &writer));
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, NULL, ZERO_LEN));
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(uds_flush_buffered_writer(writer));
  uds_free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, 0, 4, &reader));
  UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, zeros,  ZERO_LEN));
  UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  uds_free_buffered_reader(reader);
  uds_put_io_factory(factory);
  putTestBlockDevice(testDevice);
}

/**********************************************************************/
static void verifyTest(void)
{
  int i;
  static const u8 X1[] = "xxxxxx1";
  static const u8 X2[] = "xxxxxx2";
  enum { X1_LEN = sizeof(X1) - 1 };
  enum { X2_LEN = sizeof(X2) - 1 };
  enum { COUNT = UDS_BLOCK_SIZE / X1_LEN };
  struct io_factory *factory;
  struct block_device *testDevice = getTestBlockDevice();
  UDS_ASSERT_SUCCESS(uds_make_io_factory(testDevice, &factory));

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(uds_make_buffered_writer(factory, 0, 4, &writer));
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  for (i = 0; i < COUNT; i++) {
    UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, X1, X1_LEN));
    UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, X2, X2_LEN));
  }
  UDS_ASSERT_SUCCESS(uds_write_to_buffered_writer(writer, BOSTON, BOSTON_LEN));
  UDS_ASSERT_SUCCESS(uds_flush_buffered_writer(writer));
  uds_free_buffered_writer(writer);

  // check file contents, using a buffered reader
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, 0, 4, &reader));
  UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  for (i = 0; i < COUNT; i++) {
    UDS_ASSERT_ERROR(UDS_CORRUPT_DATA, uds_verify_buffered_data(reader, X2, X2_LEN));
    UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, X1, X1_LEN));
    UDS_ASSERT_ERROR(UDS_CORRUPT_DATA, uds_verify_buffered_data(reader, X1, X1_LEN));
    UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, X2, X2_LEN));
  }
  UDS_ASSERT_SUCCESS(uds_verify_buffered_data(reader, BOSTON, BOSTON_LEN));
  uds_free_buffered_reader(reader);
  uds_put_io_factory(factory);
  putTestBlockDevice(testDevice);
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
