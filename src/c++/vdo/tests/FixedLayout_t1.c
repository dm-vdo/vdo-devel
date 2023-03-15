/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdlib.h>

#include "albtest.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "constants.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-layout.h"

#include "ramLayer.h"
#include "vdoAsserts.h"

const block_count_t BLOCK_COUNT = 1024;

enum {
  BUFFER_COUNT = 6,
};

static PhysicalLayer *layer;
static char          *buffers[BUFFER_COUNT];
static int            lastBuffer;

// A captured encoding of the layout created in persistenceTest(), used to
// check that the encoding format hasn't changed and is platform-independent.
static u8 EXPECTED_LAYOUT_3_0_ENCODING[] =
  {
    0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x75, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x17, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x23,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00
  };

/**
 * Convert a physical block number to use the proper base for the
 * partition in which it will be used.
 *
 * @param partitionNumber  The number of the partition
 * @param blockNumber      The 0-based number of the block
 *
 * @return The pbn in the base of the partition
 **/
static physical_block_number_t inBase(struct partition    *partition,
                                      physical_block_number_t  blockNumber)
{
  return blockNumber + vdo_get_fixed_layout_partition_base(partition);
}

/**
 * Initialize test data structures.
 **/
static void initializeLayoutTest(void)
{
  VDO_ASSERT_SUCCESS(makeRAMLayer(BLOCK_COUNT, false, &layer));

  for (lastBuffer = 0; lastBuffer < BUFFER_COUNT; lastBuffer++) {
    char *tmp;
    UDS_ASSERT_SUCCESS(UDS_ALLOCATE(VDO_BLOCK_SIZE, char, "test buffer",
                                    &tmp));
    buffers[lastBuffer] = tmp;
    memset(buffers[lastBuffer], 'A' + lastBuffer, VDO_BLOCK_SIZE);
  }
}

/**
 * Clean up test data structures.
 **/
static void tearDownLayoutTest(void)
{
  while (lastBuffer > 0) {
    lastBuffer--;
    UDS_FREE(buffers[lastBuffer]);
  }
  layer->destroy(&layer);
}

/**********************************************************************/
static void makeAndRetrievePartition(struct fixed_layout       *layout,
                                     u8                         id,
                                     block_count_t              size,
                                     enum partition_direction   direction,
                                     physical_block_number_t    base,
                                     physical_block_number_t    expectedOffset,
                                     struct partition         **partitionPtr)
{
  CU_ASSERT_EQUAL(VDO_UNKNOWN_PARTITION,
                  vdo_get_fixed_layout_partition(layout, id, partitionPtr));
  block_count_t expectedSize = ((size == VDO_ALL_FREE_BLOCKS)
                                ? vdo_get_fixed_layout_blocks_available(layout)
                                : size);
  block_count_t expectedFreeSpace
    = vdo_get_fixed_layout_blocks_available(layout) - expectedSize;
  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout_partition(layout, id, size,
                                                     direction, base));
  CU_ASSERT_EQUAL(expectedFreeSpace,
                  vdo_get_fixed_layout_blocks_available(layout));
  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout, id, partitionPtr));
  CU_ASSERT_EQUAL(expectedSize,
                  vdo_get_fixed_layout_partition_size(*partitionPtr));
  CU_ASSERT_EQUAL(expectedOffset,
                  vdo_get_fixed_layout_partition_offset(*partitionPtr));
}

/**********************************************************************/
static void *getBuffer(unsigned int bufferIndex)
{
  return buffers[bufferIndex];
}

/**********************************************************************/
static void verifyBuffer(PhysicalLayer           *layer,
                         physical_block_number_t  startBlock,
                         unsigned int             bufferIndex)
{
  char buffer[VDO_BLOCK_SIZE];

  VDO_ASSERT_SUCCESS(layer->reader(layer, startBlock, 1, buffer));
  UDS_ASSERT_EQUAL_BYTES(buffer, getBuffer(bufferIndex), VDO_BLOCK_SIZE);
}

/**********************************************************************/
static void verifyPartition(PhysicalLayer           *layer,
                            struct partition        *partition,
                            physical_block_number_t  startBlock,
                            unsigned int             bufferIndex)
{
  startBlock = inBase(partition, startBlock);
  physical_block_number_t pbn;
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(partition, startBlock, &pbn));
  CU_ASSERT_EQUAL(pbn, (startBlock
                        + vdo_get_fixed_layout_partition_offset(partition)
                        - vdo_get_fixed_layout_partition_base(partition)));
  physical_block_number_t translatedPBN;
  VDO_ASSERT_SUCCESS(vdo_translate_from_pbn(partition, pbn, &translatedPBN));
  CU_ASSERT_EQUAL(translatedPBN, startBlock);
  verifyBuffer(layer, pbn, bufferIndex);
}

/**
 * Basic test for fixed_layout.
 **/
static void basicTest(void)
{
  // phase 1 -- setup
  struct fixed_layout *layout;
  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout(30, 1, &layout));
  CU_ASSERT_EQUAL(30, vdo_get_fixed_layout_blocks_available(layout));
  CU_ASSERT_EQUAL(30, vdo_get_total_fixed_layout_size(layout));

  struct partition *part1;
  makeAndRetrievePartition(layout, VDO_TEST_PARTITION_1, 10,
                           VDO_PARTITION_FROM_BEGINNING, 0, 1, &part1);

  CU_ASSERT_EQUAL(VDO_PARTITION_EXISTS,
                  vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_1,
                                                  10, VDO_PARTITION_FROM_END,
                                                  0));
  CU_ASSERT_EQUAL(30, vdo_get_total_fixed_layout_size(layout));

  struct partition *part2;
  makeAndRetrievePartition(layout, VDO_TEST_PARTITION_2, 10,
                           VDO_PARTITION_FROM_END, 1, 21, &part2);
  CU_ASSERT_EQUAL(VDO_NO_SPACE,
                  vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_3,
                                                  11,
                                                  VDO_PARTITION_FROM_BEGINNING,
                                                  0));
  CU_ASSERT_EQUAL(30, vdo_get_total_fixed_layout_size(layout));

  struct partition *part3;
  makeAndRetrievePartition(layout, VDO_TEST_PARTITION_3, 5,
                           VDO_PARTITION_FROM_END, 2, 16, &part3);
  CU_ASSERT_EQUAL(30, vdo_get_total_fixed_layout_size(layout));

  struct partition *part4;
  makeAndRetrievePartition(layout, VDO_TEST_PARTITION_4, VDO_ALL_FREE_BLOCKS,
                           VDO_PARTITION_FROM_BEGINNING, 3, 11, &part4);
  CU_ASSERT_EQUAL(30, vdo_get_total_fixed_layout_size(layout));

  // phase 2 -- usage
  physical_block_number_t pbn;
  // 0 + 1
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part1, inBase(part1, 0), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(0)));
  // 1 + 21
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part2, inBase(part2, 1), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(1)));
  // 2 + 16
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part3, inBase(part3, 2), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(2)));
  // 3 + 11
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part4, inBase(part4, 3), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(3)));
  // 1 + 1
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part1, inBase(part1, 1), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(4)));
  // 5 + 21
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(part2, inBase(part2, 5), &pbn));
  VDO_ASSERT_SUCCESS(layer->writer(layer, pbn, 1, getBuffer(5)));

  verifyBuffer(layer,  1, 0);
  verifyBuffer(layer, 22, 1);
  verifyBuffer(layer, 18, 2);
  verifyBuffer(layer, 14, 3);
  verifyBuffer(layer,  2, 4);
  verifyBuffer(layer, 26, 5);

  verifyPartition(layer, part1, 0, 0);
  verifyPartition(layer, part2, 1, 1);
  verifyPartition(layer, part3, 2, 2);
  verifyPartition(layer, part4, 3, 3);
  verifyPartition(layer, part1, 1, 4);
  verifyPartition(layer, part2, 5, 5);

  // phase 3 -- cleanup
  vdo_free_fixed_layout(layout);
}

/**********************************************************************/
static void checkPartition(struct fixed_layout     *layout,
                           u8                       id,
                           physical_block_number_t  expectedOffset,
                           block_count_t            expectedSize,
                           physical_block_number_t  expectedBase)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout, id, &partition));
  CU_ASSERT_EQUAL(expectedOffset,
                  vdo_get_fixed_layout_partition_offset(partition));
  CU_ASSERT_EQUAL(expectedSize,
                  vdo_get_fixed_layout_partition_size(partition));
  CU_ASSERT_EQUAL(expectedBase,
                  vdo_get_fixed_layout_partition_base(partition));
}

/**********************************************************************/
static void persistenceTest(void)
{
  block_count_t           BLOCKS      = 32;
  physical_block_number_t FIRST_BLOCK = 7;

  struct fixed_layout *layout;
  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout(BLOCKS, FIRST_BLOCK, &layout));

  int result
    = vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_1, 8,
                                      VDO_PARTITION_FROM_BEGINNING, 0);
  VDO_ASSERT_SUCCESS(result);

  result
    = vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_2, 8,
                                      VDO_PARTITION_FROM_BEGINNING, 1);
  VDO_ASSERT_SUCCESS(result);

  result
    = vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_3, 4,
                                      VDO_PARTITION_FROM_END, 2);
  VDO_ASSERT_SUCCESS(result);

  result
    = vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_4, 4,
                                      VDO_PARTITION_FROM_BEGINNING, 3);
  VDO_ASSERT_SUCCESS(result);

  CU_ASSERT_EQUAL(8, vdo_get_fixed_layout_blocks_available(layout));

  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(uds_make_buffer(vdo_get_fixed_layout_encoded_size(layout),
                                     &buffer));
  VDO_ASSERT_SUCCESS(vdo_encode_fixed_layout(layout, buffer));

  vdo_free_fixed_layout(UDS_FORGET(layout));

  // Check that the version 3.0 encoding hasn't accidentally been changed,
  // either due to code changes or because of the test platform's endianness.
  CU_ASSERT_EQUAL(sizeof(EXPECTED_LAYOUT_3_0_ENCODING),
                  uds_content_length(buffer));
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_LAYOUT_3_0_ENCODING,
                         uds_get_buffer_contents(buffer),
                         uds_content_length(buffer));

  VDO_ASSERT_SUCCESS(vdo_decode_fixed_layout(buffer, &layout));

  CU_ASSERT_EQUAL(8, vdo_get_fixed_layout_blocks_available(layout));
  CU_ASSERT_EQUAL(BLOCKS, vdo_get_total_fixed_layout_size(layout));

  checkPartition(layout, VDO_TEST_PARTITION_1, FIRST_BLOCK, 8, 0);
  checkPartition(layout, VDO_TEST_PARTITION_2, FIRST_BLOCK + 8, 8, 1);
  checkPartition(layout, VDO_TEST_PARTITION_3, FIRST_BLOCK + BLOCKS - 4, 4, 2);
  checkPartition(layout, VDO_TEST_PARTITION_4, FIRST_BLOCK + 8 + 8, 4, 3);

  result
    = vdo_make_fixed_layout_partition(layout, VDO_TEST_PARTITION_5,
                                      VDO_ALL_FREE_BLOCKS,
                                      VDO_PARTITION_FROM_BEGINNING, 4);
  VDO_ASSERT_SUCCESS(result);

  checkPartition(layout, VDO_TEST_PARTITION_5, FIRST_BLOCK + 8 + 8 + 4, 8, 4);
  CU_ASSERT_EQUAL(BLOCKS, vdo_get_total_fixed_layout_size(layout));

  uds_free_buffer(UDS_FORGET(buffer));
  vdo_free_fixed_layout(layout);
}

/**********************************************************************/
static CU_TestInfo fixedLayoutTests[] = {
  { "basic",        basicTest },
  { "save/restore", persistenceTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo fixedLayoutSuite = {
  .name                     = "Trivial fixedLayout tests (FixedLayout_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeLayoutTest,
  .cleaner                  = tearDownLayoutTest,
  .tests                    = fixedLayoutTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &fixedLayoutSuite;
}
