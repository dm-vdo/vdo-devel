/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef FILE_LAYER_H
#define FILE_LAYER_H

#include "physicalLayer.h"

/**
 * Make a file layer implementation of a physical layer.
 *
 * @param [in]  name        the name of the underlying file
 * @param [in]  blockCount  the span of the file, in blocks
 * @param [out] layerPtr    the pointer to hold the result
 *
 * @return a success or error code
 **/
int __must_check makeFileLayer(const char *name,
			       block_count_t blockCount,
			       PhysicalLayer **layerPtr);

/**
 * Make a read only file layer implementation of a physical layer.
 *
 * @param [in]  name        the name of the underlying file
 * @param [out] layerPtr    the pointer to hold the result
 *
 * @return a success or error code
 **/
int __must_check
makeReadOnlyFileLayer(const char *name, PhysicalLayer **layerPtr);

/**
 * Make an offset file layer implementation of a physical layer.
 *
 * @param [in]  name        the name of the underlying file
 * @param [in]  blockCount  the span of the file, in blocks
 * @param [in]  fileOffset  the block offset to apply to I/O operations
 * @param [out] layerPtr    the pointer to hold the result
 *
 * @return a success or error code
 **/
int makeOffsetFileLayer(const char     *name,
                        block_count_t   blockCount,
                        block_count_t   fileOffset,
                        PhysicalLayer **layerPtr)
  __attribute__((warn_unused_result));

#endif // FILE_LAYER_H
