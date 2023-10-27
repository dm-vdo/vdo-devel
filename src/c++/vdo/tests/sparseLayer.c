/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "sparseLayer.h"

#include <stdlib.h>

#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "syscalls.h"

#include "constants.h"
#include "status-codes.h"

#include "vdoAsserts.h"

/**********************************************************************/
SparseLayer *asSparseLayer(PhysicalLayer *layer)
{
  STATIC_ASSERT(offsetof(SparseLayer, common) == 0);
  return (SparseLayer *) layer;
}

/**********************************************************************/
static block_count_t getBlockCount(PhysicalLayer *header)
{
  return asSparseLayer(header)->blockCount;
}

/**
 * Implements buffer_allocator.
 **/
static int allocateIOBuffer(PhysicalLayer  *header,
                            size_t          bytes,
                            const char     *why,
                            char          **bufferPtr)
{
  SparseLayer *layer = asSparseLayer(header);
  return layer->fileLayer->allocateIOBuffer(layer->fileLayer, bytes,
                                            why, bufferPtr);
}

/**********************************************************************/
static int sparseReader(PhysicalLayer           *header,
                        physical_block_number_t  startBlock,
                        size_t                   blockCount,
                        char                    *buffer)
{
  SparseLayer *layer = asSparseLayer(header);

  physical_block_number_t index = startBlock;
  if ((index + blockCount) > layer->blockCount) {
    return VDO_OUT_OF_RANGE;
  }

  size_t blocksLeft = blockCount;
  char *bufferPtr   = buffer;

  for (unsigned int rangeIndex = 0;
       (rangeIndex < layer->numRanges) && (blocksLeft > 0); rangeIndex++) {

    MappingRange *currentRange = &layer->ranges[rangeIndex];
    if (startBlock > (currentRange->start + currentRange->length)) {
      continue;
    }

    // Read zeros for blocks between mapped ranges.
    if (startBlock < currentRange->start) {
      block_count_t emptyBlocks;
      emptyBlocks = min(currentRange->start - startBlock, blocksLeft);
      size_t emptyBytes = emptyBlocks * VDO_BLOCK_SIZE;
      memset(bufferPtr, 0, emptyBytes);
      blocksLeft -= emptyBlocks;
      startBlock += emptyBlocks;
      bufferPtr += emptyBytes;
    }

    if (blocksLeft == 0) {
      continue;
    }

    // Read blocks in the range from the file layer.
    physical_block_number_t realBlocks
      = min(currentRange->start + currentRange->length - startBlock,
            blocksLeft);
    physical_block_number_t fileLayerStart
      = startBlock - currentRange->start + currentRange->offset;
    layer->fileLayer->reader(layer->fileLayer, fileLayerStart, realBlocks,
                             bufferPtr);
    blocksLeft -= realBlocks;
    startBlock += realBlocks;
    bufferPtr += (realBlocks * VDO_BLOCK_SIZE);
  }

  // Read zeros for blocks outside of all ranges.
  if (blocksLeft > 0) {
    memset(bufferPtr, 0, blocksLeft * VDO_BLOCK_SIZE);
  }

  return VDO_SUCCESS;
}

/**********************************************************************/
static int sparseWriter(PhysicalLayer           *header,
                        physical_block_number_t  startBlock,
                        size_t                   blockCount,
                        char                    *buffer)
{
  SparseLayer *layer = asSparseLayer(header);

  physical_block_number_t index = startBlock;
  if ((index + blockCount) > layer->blockCount) {
    return VDO_OUT_OF_RANGE;
  }

  size_t blocksLeft = blockCount;
  char * bufferPtr = buffer;
  for (unsigned int rangeIndex = 0;
       (rangeIndex < layer->numRanges) && (blocksLeft > 0); rangeIndex++) {

    MappingRange *currentRange = &layer->ranges[rangeIndex];
    if (startBlock > (currentRange->start + currentRange->length)) {
      continue;
    }

    // Blocks between ranges can't be written, but must be counted.
    if (startBlock < currentRange->start) {
      block_count_t emptyBlocks;
      emptyBlocks = min(currentRange->start - startBlock, blocksLeft);
      blocksLeft -= emptyBlocks;
      startBlock += emptyBlocks;
      size_t emptyBytes = emptyBlocks * VDO_BLOCK_SIZE;
      bufferPtr += emptyBytes;
    }

    if (blocksLeft == 0) {
      continue;
    }

    // Write actual data in range.
    physical_block_number_t realBlocks
      = min(currentRange->start + currentRange->length - startBlock,
            blocksLeft);
    physical_block_number_t fileLayerStart
      = startBlock - currentRange->start + currentRange->offset;
    layer->fileLayer->writer(layer->fileLayer, fileLayerStart, realBlocks,
                             bufferPtr);
    blocksLeft -= realBlocks;
    startBlock += realBlocks;
    bufferPtr += (realBlocks * VDO_BLOCK_SIZE);
  }

  // Anything left is beyond the last range and can be ignored.
  return VDO_SUCCESS;
}

/**********************************************************************/
static int verifyRanges(block_count_t   blockCount,
                        unsigned int    numRanges,
                        MappingRange   *ranges)
{
  block_count_t firstUncommittedBlock = 0;
  for (unsigned int rangeIndex = 0; rangeIndex < numRanges; rangeIndex++) {
    MappingRange *range = &ranges[rangeIndex];
    if (range->start < firstUncommittedBlock) {
      return VDO_OUT_OF_RANGE;
    }

    firstUncommittedBlock = range->start + range->length;
    if (firstUncommittedBlock > blockCount) {
      return VDO_OUT_OF_RANGE;
    }
  }

  return VDO_SUCCESS;
}

/**
 * Free a sparse layer.
 *
 * @param layer  The layer to free
 **/
static void freeSparseLayer(SparseLayer *layer)
{
  if (layer == NULL) {
    return;
  }

  layer->fileLayer->destroy(&layer->fileLayer);
  if (layer->name != NULL) {
    unlink(layer->name);
  }
  uds_free(layer->name);
  uds_free(layer->ranges);
  uds_free(layer);
}

/**
 * Free a spare layer and NULL out the reference to it.
 *
 * Implements layer_destructor.
 *
 * @param layerPtr  A pointer to the layer to destroy
 **/
static void freeLayer(PhysicalLayer **layerPtr)
{
  PhysicalLayer *layer = *layerPtr;
  if (layer == NULL) {
    return;
  }

  freeSparseLayer(asSparseLayer(layer));
  *layerPtr = NULL;
}

/**********************************************************************/
int makeSparseLayer(const char     *name,
                    block_count_t   blockCount,
                    unsigned int    numRanges,
                    MappingRange   *ranges,
                    PhysicalLayer **layerPtr)
{
  SparseLayer *layer;
  int result = UDS_ALLOCATE(1, SparseLayer, __func__, &layer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = verifyRanges(blockCount, numRanges, ranges);
  if (result != UDS_SUCCESS) {
    freeSparseLayer(layer);
    return result;
  }

  result = UDS_ALLOCATE(numRanges, MappingRange, __func__, &layer->ranges);
  if (result != UDS_SUCCESS) {
    freeSparseLayer(layer);
    return result;
  }

  size_t nameLength = strlen(name) + 1;
  result = UDS_ALLOCATE(nameLength, char, __func__, &layer->name);
  if (result != UDS_SUCCESS) {
    return result;
  }
  strcpy(layer->name, name);

  block_count_t fileLayerBlockCount = 0;
  for (unsigned int rangeIndex = 0; rangeIndex < numRanges; rangeIndex++) {
    // count up size of file layer
    fileLayerBlockCount += ranges[rangeIndex].length;
  }

  // Create a file for the underlying layer.
  unlink(layer->name);
  char *command;
  int bytesWritten = asprintf(&command, "dd if=/dev/zero of=%s bs=%d count=1",
                              layer->name, VDO_BLOCK_SIZE);
  CU_ASSERT_TRUE(bytesWritten > 0);
  VDO_ASSERT_SUCCESS(system(command));
  VDO_ASSERT_SUCCESS(truncate(layer->name,
                              VDO_BLOCK_SIZE * fileLayerBlockCount));
  free(command);

  result = makeFileLayer(layer->name, fileLayerBlockCount,
                         &layer->fileLayer);
  if (result != UDS_SUCCESS) {
    freeSparseLayer(layer);
    return result;
  }

  memcpy(layer->ranges, ranges, numRanges * sizeof(MappingRange));
  layer->blockCount = blockCount;
  layer->numRanges  = numRanges;

  layer->common.destroy          = freeLayer;
  layer->common.getBlockCount    = getBlockCount;
  layer->common.allocateIOBuffer = allocateIOBuffer;
  layer->common.reader           = sparseReader;
  layer->common.writer           = sparseWriter;

  *layerPtr = &layer->common;
  return VDO_SUCCESS;
}
