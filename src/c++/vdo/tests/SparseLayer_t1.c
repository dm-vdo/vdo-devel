/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "fileUtils.h"
#include "memory-alloc.h"
#include "syscalls.h"

#include "constants.h"

#include "sparseLayer.h"
#include "vdoAsserts.h"

enum {
  BLOCK_COUNT  = 64,
  MAPPED_COUNT = 10,
  RANGE_COUNT  = 3,
};

const char    *testFile = "sparse_test_file";
char          *testData = NULL;
PhysicalLayer *layer    = NULL;

/**
 * Initialize the test data buffer and sparse layer.
 **/
static void initializeData(void)
{
  MappingRange ranges[RANGE_COUNT] = {
    {
      .start  = 10,
      .length = MAPPED_COUNT,
      .offset = 0,
    },
    {
      .start  = 30,
      .length = MAPPED_COUNT,
      .offset = MAPPED_COUNT,
    },
    {
      .start  = 50,
      .length = MAPPED_COUNT,
      .offset = MAPPED_COUNT + MAPPED_COUNT,
    },
  };

  VDO_ASSERT_SUCCESS(makeSparseLayer(testFile, BLOCK_COUNT, 3, ranges,
                                     &layer));

  size_t dataSize = VDO_BLOCK_SIZE * BLOCK_COUNT;
  VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, dataSize, "test data",
                                             &testData));
  for (unsigned int i = 0; i < BLOCK_COUNT; i++) {
    memset(testData + (VDO_BLOCK_SIZE * i), i, VDO_BLOCK_SIZE);
  }
}

/**
 * Destroy the test data and sprase layer.
 **/
static void tearDownData(void)
{
  free(testData);
  layer->destroy(&layer);
  unlink(testFile);
}

/**
 * Simple test of a sparse layer.
 **/
static void testBasic(void)
{
  char *buffer;
  size_t dataSize = VDO_BLOCK_SIZE * BLOCK_COUNT;
  VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, dataSize, "buffer",
                                             &buffer));

  // Write every block in the layer.
  VDO_ASSERT_SUCCESS(layer->writer(layer, 0, BLOCK_COUNT, testData));

  // Read every block in the layer.
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, BLOCK_COUNT, buffer));

  // Verify that blocks in mapped ranges match.
  for (unsigned int index = 0; index < RANGE_COUNT; index++) {
    MappingRange *ranges = asSparseLayer(layer)->ranges;
    size_t offset = ranges[index].start * VDO_BLOCK_SIZE;
    size_t bytes  = ranges[index].length * VDO_BLOCK_SIZE;
    UDS_ASSERT_EQUAL_BYTES(buffer + offset, testData + offset, bytes);
  }

  // Test that the underlying file is not too large.
  int fd = 0;
  VDO_ASSERT_SUCCESS(open_file(testFile, FU_READ_ONLY, &fd));
  off_t fileSize = 0;
  VDO_ASSERT_SUCCESS(get_open_file_size(fd, &fileSize));
  VDO_ASSERT_SUCCESS(close_file(fd, NULL));
  CU_ASSERT_EQUAL(fileSize, MAPPED_COUNT * RANGE_COUNT * VDO_BLOCK_SIZE);

  UDS_FREE(buffer);
}

/**********************************************************************/
static CU_TestInfo sparseLayerTests[] = {
  { "basic ",    testBasic    },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo sparseLayerSuite = {
  .name                     = "Sparse Layer tests (SparseLayer_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeData,
  .cleaner                  = tearDownData,
  .tests                    = sparseLayerTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &sparseLayerSuite;
}
