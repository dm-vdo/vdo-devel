// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

enum { DATA_BLOCKS = 8 };
enum { DATA_SIZE   = DATA_BLOCKS * UDS_BLOCK_SIZE };

static u8                *data;
static struct io_factory *factory;

/**********************************************************************/
static void createAndWriteData(void)
{
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(DATA_SIZE, u8, __func__, &data));
  get_random_bytes(data, DATA_SIZE);

  UDS_ASSERT_SUCCESS(uds_make_io_factory(getTestIndexName(), &factory));
  struct dm_bufio_client *client = NULL;
  UDS_ASSERT_SUCCESS(uds_make_bufio(factory, 0, UDS_BLOCK_SIZE, 1, &client));
  int i;
  for (i = 0; i < DATA_BLOCKS; i++) {
    struct dm_buffer *buffer = NULL;
    void *bufdata = dm_bufio_new(client, i, &buffer);
    UDS_ASSERT_KERNEL_SUCCESS(bufdata);
    memcpy(bufdata, &data[i * UDS_BLOCK_SIZE], UDS_BLOCK_SIZE);
    dm_bufio_mark_buffer_dirty(buffer);
    dm_bufio_release(buffer);
  }
  dm_bufio_client_destroy(client);
}

/**********************************************************************/
static void verifyData(int count)
{
  int offset;
  u8 *buf;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count, u8, __func__, &buf));

  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(uds_make_buffered_reader(factory, 0, DATA_BLOCKS, &reader));

  for (offset = 0; offset + count <= DATA_SIZE; offset += count) {
    UDS_ASSERT_SUCCESS(uds_read_from_buffered_reader(reader, buf, count));
    UDS_ASSERT_EQUAL_BYTES(&data[offset], buf, count);
  }

  UDS_ASSERT_ERROR(UDS_OUT_OF_RANGE,
                   uds_read_from_buffered_reader(reader, buf, count));
  uds_free_buffered_reader(reader);
  UDS_FREE(buf);
}

/**********************************************************************/
static void freeEverything(void)
{
  uds_put_io_factory(factory);
  UDS_FREE(data);
  data    = NULL;
  factory = NULL;
}

/**********************************************************************/
static void readerTest(void)
{
  createAndWriteData();
  verifyData(4);
  verifyData(5);
  uds_put_io_factory(factory);
  UDS_ASSERT_SUCCESS(uds_make_io_factory(getTestIndexName(), &factory));
  verifyData(2 * UDS_BLOCK_SIZE);
  verifyData(42);
  freeEverything();
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "reader", readerTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "BufferedReader_t1",
  .tests = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
