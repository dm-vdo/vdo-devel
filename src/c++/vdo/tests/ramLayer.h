/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef RAM_LAYER_H
#define RAM_LAYER_H

#include "types.h"

#include "physicalLayer.h"

typedef struct ramLayer RAMLayer;

/**
 * A function to check a mismatched block when comparing layers. It should fail
 * the test if the mismatch is not acceptable.
 *
 * @param pbn            The block number with the mismatch
 * @param expectedBlock  The expected contents of the pbn
 * @param actualBlock    The actual contents of the pbn
 **/
typedef void MismatchChecker(physical_block_number_t  pbn,
                             char                    *expectedBlock,
                             char                    *actualBlock);


/**
 * Convert a generic PhysicalLayer to a RAMLayer.
 *
 * @param layer The PhysicalLayer to convert
 *
 * @return The PhysicalLayer as a RAMLayer
 **/
RAMLayer *asRAMLayer(PhysicalLayer *layer)
  __attribute__((warn_unused_result));

/**
 * Implement a physical layer in RAM.
 *
 * @param [in]  blockCount      the span of the RAM, in blocks
 * @param [in]  acceptsFlushes  whether the layer accepts flushes
 * @param [out] layerPtr        the pointer to hold the result
 *
 * @return a success or error code
 *
 * @note the RAM will be immediately truncated to the specified size
 **/
int makeRAMLayer(block_count_t    blockCount,
                 bool		  acceptsFlushes,
                 PhysicalLayer	**layerPtr)
  __attribute__((warn_unused_result));

/**
 * Make a RAMLayer and populate it from a file.
 *
 * @param [in]  path            the path to the file
 * @param [in]  acceptsFlushes  whether the layer accepts flushes
 * @param [out] layerPtr        the pointer to hold the result
 **/
void makeRAMLayerFromFile(const char     *path,
                          bool            acceptsFlushes,
                          PhysicalLayer **layerPtr);

/**
 * Zero out a portion of a RAM Layer.
 *
 * @param layer       The layer to zero out
 * @param startBlock  The physical block number at which to start
 * @param blockCount  The number of blocks to zero
 **/
void zeroRAMLayer(PhysicalLayer *layer, physical_block_number_t startBlock, size_t blockCount);

/**
 * Resize a RAM layer.
 *
 * @param header The layer
 * @param newSize The new number of blocks the layer should have
 *
 * @return VDO_SUCCESS or an error
 **/
int resizeRAMLayer(PhysicalLayer *header, block_count_t newSize)
  __attribute__((warn_unused_result));

/**
 * Copy the contents of one RAMLayer to another RAMLayer of the same size.
 *
 * @param  to     The layer to copy to
 * @param  from   The layer to copy from
 **/
void copyRAMLayer(PhysicalLayer *to, PhysicalLayer *from);

/**
 * Clone a RAMLayer.
 *
 * @param  layer  The RAMLayer to clone
 *
 * @return The clone
 **/
PhysicalLayer *cloneRAMLayer(PhysicalLayer *layer)
  __attribute__((warn_unused_result));

/**
 * Persist a single block.
 *
 * @param layer        The layer to persist a block of
 * @param blockNumber  The block number of the block to persist
 **/
void persistSingleBlockInRAMLayer(PhysicalLayer           *layer,
                                  physical_block_number_t  blockNumber);

/**
 * Prepare to crash by preventing any subsequent writes from being persisted.
 *
 * @param layer  The layer to prepare
 **/
void prepareToCrashRAMLayer(PhysicalLayer *layer);

/**
 * Simulate a crash by losing any unflushed writes.
 *
 * @param layer  The layer to crash
 **/
void crashRAMLayer(PhysicalLayer *layer);

/**
 * Dump a RAMLayer to a file.
 *
 * @param layer  The layer to dump
 * @param fd     The file descriptor to dump to
 **/
void dumpRAMLayerToFile(PhysicalLayer *layer, int fd);

/**
 * Check the contents of a RAMLayer
 *
 * @param layer             The layer to check
 * @param expectedContents  The expected contents of the layer
 * @param checker           A function which will be called on each mismatched
 *                          block
 **/
void checkRAMLayerContents(PhysicalLayer *layer,
                           char *expectedContents,
                           MismatchChecker checker);

/**
 * Make all writes persistent.
 *
 * @param layer  The layer to flush
 **/
void flushRAMLayer(PhysicalLayer *layer);

#endif // RAM_LAYER_H
