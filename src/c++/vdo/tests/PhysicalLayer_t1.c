/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <stdlib.h>

#include "memory-alloc.h"

#include "constants.h"

#include "fileLayer.h"

#include "ramLayer.h"
#include "vdoAsserts.h"

enum {
  BLOCK_COUNT = 64,
};

static bool isFileLayer;

/**
 * Fill a block-sized buffer with sequential data based on key.
 *
 * @param buf   a block-sized buffer
 * @param key   the key used to generate the data
 **/
static void fillBuf(char *buf, unsigned int key)
{
  char *data = buf;
  unsigned char c = key % 256;
  unsigned char s = (key / 256) + 1;

  for (char *d = data; d < data + VDO_BLOCK_SIZE; ++d) {
    *d = c;
    c += s;
  }
}

/**
 * Verify that the specified data is read from the layer.
 *
 *
 * @param data   the buffer of data
 * @param layer  the layer to read from
 * @param start  the starting block number
 * @param count  the block count
 **/
static void verifyLayerRead(char                    *data,
                            PhysicalLayer           *layer,
                            physical_block_number_t  start,
                            block_count_t            count)
{
  size_t bufferBytes = count * VDO_BLOCK_SIZE;
  char *buf;
  VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, bufferBytes, "buffer",
                                             &buf));
  memset(buf, 255, bufferBytes);

  VDO_ASSERT_SUCCESS(layer->reader(layer, start, count, buf));
  CU_ASSERT_EQUAL(memcmp(data, buf, bufferBytes), 0);
  vdo_free(buf);

  // Also check an unaligned buffer if the layer is a file layer
  if (!isFileLayer) {
    return;
  }

  VDO_ASSERT_SUCCESS(vdo_allocate(bufferBytes, char, __func__, &buf));
  memset(buf, 255, bufferBytes);
  VDO_ASSERT_SUCCESS(layer->reader(layer, start, count, buf));
  CU_ASSERT_EQUAL(memcmp(data, buf, bufferBytes), 0);
  vdo_free(buf);
}

/**
 * Write the specified data and verify the data is read back.
 *
 *
 * @param data   the buffer of data
 * @param layer  the layer to write, then read from
 * @param start  the starting block number
 * @param count  the block count
 **/
static void verifyLayerWrite(char		      *data,
                             PhysicalLayer	      *layer,
                             physical_block_number_t  start,
                             block_count_t            count)
{
  VDO_ASSERT_SUCCESS(layer->writer(layer, start, count, data));
  verifyLayerRead(data, layer, start, count);

  // Also check an unaligned buffer if the layer is a file layer
  char *buffer;
  VDO_ASSERT_SUCCESS(vdo_allocate(count * VDO_BLOCK_SIZE, char, __func__,
                                  &buffer));
  VDO_ASSERT_SUCCESS(layer->writer(layer, start, count, buffer));
  memcpy(buffer, data, count * VDO_BLOCK_SIZE);
  VDO_ASSERT_SUCCESS(layer->writer(layer, start, count, buffer));
  vdo_free(buffer);
  verifyLayerRead(data, layer, start, count);
}

/**********************************************************************/
static void checkPersistentLayer(PhysicalLayer **layerPtr)
{
  PhysicalLayer *layer = *layerPtr;
  for (block_count_t b = BLOCK_COUNT - 1; ; --b) {
    char *buf;
    VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "buffer",
                                               &buf));
    // Blocks at offset 1 mod 7 use a different key.
    fillBuf(buf, (b % 7 == 1) ? b + 1001 : b);
    verifyLayerRead(buf, layer, b, 1);
    vdo_free(buf);
    if (b == 0) {
      break;
    }
  }

  layer->destroy(layerPtr);
}

/**********************************************************************/
static void checkGenericLayer(PhysicalLayer **layerPtr)
{
  PhysicalLayer *layer = *layerPtr;
  char *zeros;
  size_t bytes = VDO_BLOCK_SIZE * BLOCK_COUNT;
  VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, bytes, "zeroes", &zeros));

  CU_ASSERT_EQUAL(BLOCK_COUNT, layer->getBlockCount(layer));

  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE,
                  layer->writer(layer, BLOCK_COUNT, 1, zeros));
  vdo_free(zeros);

  // Write sequential data.
  for (block_count_t b = 0; b < BLOCK_COUNT; ++b) {
    char *buf;
    VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE,
                                               "buffer", &buf));
    fillBuf(buf, b);
    verifyLayerWrite(buf, layer, b, 1);
    vdo_free(buf);
  }

  // Overwrite every seventh block starting at 1 with
  // different data.
  for (block_count_t b = 1; b < BLOCK_COUNT; b += 7) {
    char *buf;
    VDO_ASSERT_SUCCESS(layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE,
                                               "buffer", &buf));
    fillBuf(buf, b + 1001);
    verifyLayerWrite(buf, layer, b, 1);
    vdo_free(buf);
  }

  checkPersistentLayer(layerPtr);
}

/**
 * simple test of ram layer
 **/
static void ramLayerTest(void)
{
  PhysicalLayer *layer;
  VDO_ASSERT_SUCCESS(makeRAMLayer(BLOCK_COUNT, false, &layer));
  isFileLayer = false;
  checkGenericLayer(&layer);
  CU_ASSERT_PTR_NULL(layer);
}

/**
 * Simple test of file layer
 **/
static void fileLayerTest(void)
{
  const char filename[] = "test_file";
  unlink(filename);

  char *command;
  int bytesWritten = asprintf(&command, "dd if=/dev/zero of=%s bs=%d count=%d",
                              filename, VDO_BLOCK_SIZE, BLOCK_COUNT);
  CU_ASSERT_TRUE(bytesWritten > 0);
  VDO_ASSERT_SUCCESS(system(command));
  free(command);

  PhysicalLayer *layer;
  VDO_ASSERT_SUCCESS(makeFileLayer(filename, BLOCK_COUNT, &layer));
  isFileLayer = true;
  checkGenericLayer(&layer);
  CU_ASSERT_PTR_NULL(layer);

  VDO_ASSERT_SUCCESS(makeFileLayer(filename, BLOCK_COUNT, &layer));
  checkPersistentLayer(&layer);
  unlink(filename);
}

/**********************************************************************/

static CU_TestInfo physicalLayerTests[] = {
  { "ramLayer",                      ramLayerTest                  },
  { "fileLayer",                     fileLayerTest                 },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo physicalLayerSuite = {
  .name                     = "Generic PhysicalLayer tests (PhysicalLayer_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = physicalLayerTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &physicalLayerSuite;
}
