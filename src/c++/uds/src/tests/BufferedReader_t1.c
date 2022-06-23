// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"

enum { DATA_BLOCKS = 8 };
enum { DATA_SIZE   = DATA_BLOCKS * UDS_BLOCK_SIZE };

static byte              *data;
static struct io_factory *factory;

/**********************************************************************/
static void createAndWriteData(void)
{
  factory = getTestIOFactory();

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(DATA_SIZE, byte, __func__, &data));
  fill_randomly(data, DATA_SIZE);

#ifdef __KERNEL__
  struct dm_bufio_client *client = NULL;
  UDS_ASSERT_SUCCESS(make_uds_bufio(factory, 0, UDS_BLOCK_SIZE, 1, &client));
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
#else
  struct io_region *region = NULL;
  UDS_ASSERT_SUCCESS(make_uds_io_region(factory, 0, DATA_SIZE, &region));
  UDS_ASSERT_SUCCESS(write_to_region(region, 0, data, DATA_SIZE, DATA_SIZE));
  UDS_ASSERT_SUCCESS(sync_region_contents(region));
  put_io_region(region);
#endif
}

/**********************************************************************/
static void verifyData(int count)
{
  int offset;
  byte *buf;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count, byte, __func__, &buf));

  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(open_uds_buffered_reader(factory, 0, DATA_SIZE, &reader));

  for (offset = 0; offset + count <= DATA_SIZE; offset += count) {
    UDS_ASSERT_SUCCESS(read_from_buffered_reader(reader, buf, count));
    UDS_ASSERT_EQUAL_BYTES(&data[offset], buf, count);
  }

  if (offset < DATA_SIZE) {
    UDS_ASSERT_ERROR(UDS_SHORT_READ,
                     read_from_buffered_reader(reader, buf, count));
  } else {
    UDS_ASSERT_ERROR2(UDS_OUT_OF_RANGE, UDS_END_OF_FILE,
                      read_from_buffered_reader(reader, buf, count));
  }

  free_buffered_reader(reader);
  UDS_FREE(buf);
}

/**********************************************************************/
static void freeEverything(void)
{
  put_uds_io_factory(factory);
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
  put_uds_io_factory(factory);
  factory = getTestIOFactory();
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
