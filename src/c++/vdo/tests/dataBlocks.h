/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef DATA_BLOCKS_H
#define DATA_BLOCKS_H

#include "types.h"

/**
 * A function to format test data.
 *
 * @param block  The buffer to format
 * @param index  The index of the block being filled
 **/
typedef void DataFormatter(char *block, block_count_t index);

/**
 * Fill a block with its offset as an 8 byte value.
 *
 * <p>Implements DataFormatter.
 **/
void fillWithOffset(char *block, block_count_t index);

/**
 * Fill a block with its offset plus one as an 8 byte value
 *
 * <p>Implements DataFormatter.
 **/
void fillWithOffsetPlusOne(char *block, block_count_t index);

/**
 * Fill a block with the number 47.
 *
 * <p>Implements DataFormatter.
 **/
void fillWithFortySeven(char *block, block_count_t index);

/**
 * Fill a block with ones if its index is even, and twos if its index is odd.
 *
 * <p>Implements DataFormatter.
 **/
void fillAlternating(char *block, block_count_t index);

/**
 * Initialize the data blocks for a test.
 *
 * @param formatter  The type of formatter to use
 **/
void initializeDataBlocks(DataFormatter formatter);

/**
 * Clean up data blocks for a test.
 **/
void tearDownDataBlocks(void);

/**
 * Get a formatted test data block.
 *
 * @param index  The index of the block to get
 *
 * @return The requested block
 **/
char *getDataBlock(block_count_t index)
  __attribute__((warn_unused_result));

#endif // DATA_BLOCKS_H
