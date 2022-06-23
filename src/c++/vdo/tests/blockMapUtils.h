/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef BLOCK_MAP_UTILS_H
#define BLOCK_MAP_UTILS_H

#include "kernel-types.h"
#include "types.h"

typedef void PopulateBlockMapConfigurator(struct data_vio *dataVIO);

/**
 * Initialize block map utilities.
 *
 * @param logicalBlocks  The number of logical blocks in the VDO
 **/
void initializeBlockMapUtils(block_count_t logicalBlocks);

/**
 * Free up resources allocated in initializeBlockMapUtils().
 **/
void tearDownBlockMapUtils(void);

/**
 * Populate the block map without writing data.
 *
 * @param start         The lbn of the first entry
 * @param count         The number of entries to make
 * @param configurator  A function to configure the data_vio to make the desired
 *                      block map entry
 **/
void populateBlockMap(logical_block_number_t start,
                      block_count_t count,
                      PopulateBlockMapConfigurator *configurator);

/**
 * Look up an lbn in the block map.
 *
 * @param lbn  The lbn to look up
 *
 * @return The mapping for that lbn
 **/
struct zoned_pbn lookupLBN(logical_block_number_t lbn)
  __attribute__((warn_unused_result));

/**
 * Verify that the contents of the block map match the expected
 * values in the mapping array.
 *
 * @param start  The first lbn to check
 **/
void verifyBlockMapping(logical_block_number_t start);

/**
 * Set the cached mapping of for an LBN.
 *
 * @param lbn    The lbn whose mapping is to be set
 * @param pbn    The pbn to set
 * @param state  The mapping state to set
 **/
void setBlockMapping(logical_block_number_t   lbn,
                     physical_block_number_t  pbn,
                     enum block_mapping_state state);

/**
 * Set the expected error for block accesses to the given lbn.
 *
 * @param lbn    The lbn
 * @param error  The expected error from attempting to access the specified lbn
 **/
void setBlockMappingError(logical_block_number_t lbn, int error);

#endif // BLOCK_MAP_UTILS_H
