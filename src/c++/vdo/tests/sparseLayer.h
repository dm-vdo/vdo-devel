/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef SPARSE_LAYER_H
#define SPARSE_LAYER_H

#include "fileLayer.h"
#include "physicalLayer.h"

#include "types.h"

/**
 * A SparseLayer resembles a file layer, and includes one underneath.
 * However, the Sparse Layer maps one or more ranges of physical
 * block numbers to ranges of physical blocks on the underlying file
 * layer.  Any block which is not included in a range will read as a
 * zero block, and writes to these blocks will have no effect.
 *
 * This construction allows tests to simulate a very large layer
 * without requiring the resources to actually built that layer.
 * However, care must be taken not to rely on the contents of
 * any block outside the mapped ranges.
 **/

/**
 * A description of a range of blocks to map to the underlying layer.
 **/
typedef struct {
  physical_block_number_t start;  // first block in the range
  physical_block_number_t length; // total blocks in the range
  physical_block_number_t offset; // the offset in the underlying layer where
                              //   the first block is stored
} MappingRange;

/**
 * SparseLayer internals exposed for the use of tests.
 **/
typedef struct {
  PhysicalLayer  common;
  block_count_t  blockCount;
  char          *name;
  PhysicalLayer *fileLayer;
  unsigned int   numRanges;
  MappingRange  *ranges;
} SparseLayer;

/**
 * Convert a generic PhysicalLayer to a SparseLayer.
 *
 * @param layer The PhysicalLayer to convert
 *
 * @return The PhysicalLayer as a SparseLayer
 **/
SparseLayer *asSparseLayer(PhysicalLayer *layer)
  __attribute__((warn_unused_result));

/**
 * Construct a sparse file layer.
 *
 * @param [in]  name        the name of the underlying file
 * @param [in]  blockCount  the apparent span of the layer, in blocks
 * @param [in]  numRanges   the number of defined ranges in the layer
 * @param [in]  ranges      the mapped ranges, which must be in ascending
 *                            order and must not overlap
 * @param [out] layerPtr    the pointer to hold the result
 *
 * @return a success or error code
 **/
int makeSparseLayer(const char     *name,
                    block_count_t   blockCount,
                    unsigned int    numRanges,
                    MappingRange   *ranges,
                    PhysicalLayer **layerPtr)
  __attribute__((warn_unused_result));

#endif // SPARSE_LAYER_H
