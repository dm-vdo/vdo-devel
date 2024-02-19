/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "ramLayer.h"

#include <string.h>

#include "fileUtils.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "syscalls.h"
#include "thread-utils.h"

#include "constants.h"
#include "status-codes.h"

#include "vdoAsserts.h"

enum {
  REGION_BYTES = 1 << 20,
  REGION_BLOCKS = REGION_BYTES / VDO_BLOCK_SIZE,
};

typedef uint32_t RegionNumber;

static u8 INITIAL_RAMLAYER_PATTERN = 0xfe;

typedef struct region Region;

struct region {
  bool dirty;
  char cache[REGION_BYTES];
  char data[REGION_BYTES];
  Region *next;
};

struct ramLayer {
  PhysicalLayer   common;
  block_count_t   blockCount;
  RegionNumber    regionCount;
  size_t          size;
  bool            acceptsFlushes;
  bool            writesEnabled;
  Region        **regions;
  Region         *regionList;
  int             backing;
  u8              pattern;
  struct mutex    mutex;
};

RAMLayer *asRAMLayer(PhysicalLayer *layer)
{
  STATIC_ASSERT(offsetof(RAMLayer, common) == 0);
  return (RAMLayer *) layer;
}

/**********************************************************************/
static block_count_t getBlockCount(PhysicalLayer *header)
{
  return asRAMLayer(header)->blockCount;
}

/**
 * Implements buffer_allocator.
 **/
static int allocateIOBuffer(PhysicalLayer  *layer __attribute__((unused)),
                            size_t          bytes,
                            const char     *why,
                            char          **bufferPtr)
{
  return uds_allocate(bytes, char, why, bufferPtr);
}

/**
 * Get a region, allocating it if needed and populating it from the backing
 * file if there is one. The layer mutex must be held when calling this method.
 *
 * @param layer         The layer for which to allocate a region
 * @param regionNumber  The region to get
 * @param read          If true, this is a read, and the region will only be
 *                      allocated if there is a backing file, for a write, the
 *                      region will always be allocated
 *
 * @return The region (may be NULL)
 **/
static Region *getRegion(RAMLayer *layer, RegionNumber regionNumber, bool read)
{
  Region *region = layer->regions[regionNumber];
  if ((region != NULL) || (read && (layer->backing == -1))) {
    return region;
  }

  VDO_ASSERT_SUCCESS(uds_allocate(1, Region, __func__, &region));
  layer->regions[regionNumber] = region;
  region->next = layer->regionList;
  layer->regionList = region;

  if (layer->backing == -1) {
    memset(region->data, layer->pattern, REGION_BYTES);
    memset(region->cache, layer->pattern, REGION_BYTES);
    return region;
  }

  block_count_t end
    = min((block_count_t) ((regionNumber + 1) * REGION_BLOCKS),
          (block_count_t) layer->blockCount);
  size_t to_read = (end - (regionNumber * REGION_BLOCKS)) * VDO_BLOCK_SIZE;
  size_t has_read;
  VDO_ASSERT_SUCCESS(read_data_at_offset(layer->backing,
                                         regionNumber * REGION_BYTES,
                                         layer->regions[regionNumber]->cache,
                                         to_read,
                                         &has_read));
  CU_ASSERT_EQUAL(has_read, to_read);

  // make sure the cache and data agree
  memcpy(region->data, region->cache, to_read);
  return region;
}

/**********************************************************************/
static int ramReader(PhysicalLayer           *header,
                     physical_block_number_t  startBlock,
                     size_t                   blockCount,
                     char                    *buffer)
{
  RAMLayer *layer = asRAMLayer(header);

  if ((startBlock + blockCount) > layer->blockCount) {
    return VDO_OUT_OF_RANGE;
  }

  mutex_lock(&layer->mutex);

  RegionNumber regionNumber = startBlock / REGION_BLOCKS;
  physical_block_number_t offset = startBlock % REGION_BLOCKS;
  while (blockCount > 0) {
    size_t regionBlocks = min((size_t) (REGION_BLOCKS - offset),
                              (size_t) blockCount);
    size_t regionBytes = regionBlocks * VDO_BLOCK_SIZE;
    Region *region = getRegion(layer, regionNumber, true);
    if (region == NULL) {
      memset(buffer, layer->pattern, regionBytes);
    } else {
      memcpy(buffer, region->cache + (offset * VDO_BLOCK_SIZE), regionBytes);
    }

    blockCount -= regionBlocks;
    buffer     += regionBytes;
    offset      = 0;
    regionNumber++;
  }

  mutex_unlock(&layer->mutex);

  return VDO_SUCCESS;
}

/**********************************************************************/
static int ramWriter(PhysicalLayer           *header,
                     physical_block_number_t  startBlock,
                     size_t                   blockCount,
                     char                    *buffer)
{
  RAMLayer *layer = asRAMLayer(header);

  if ((startBlock + blockCount) > layer->blockCount) {
    return VDO_OUT_OF_RANGE;
  }

  mutex_lock(&layer->mutex);

  RegionNumber regionNumber = startBlock / REGION_BLOCKS;
  physical_block_number_t offset = startBlock % REGION_BLOCKS;
  for (; blockCount > 0; regionNumber++) {
    size_t regionBlocks = min((size_t) (REGION_BLOCKS - offset),
                              (size_t) blockCount);
    size_t regionBytes = regionBlocks * VDO_BLOCK_SIZE;
    Region *region = getRegion(layer, regionNumber, false);
    memcpy(region->cache + (offset * VDO_BLOCK_SIZE), buffer, regionBytes);
    if (!layer->acceptsFlushes && layer->writesEnabled) {
      memcpy(region->data + (offset * VDO_BLOCK_SIZE), buffer, regionBytes);
    } else {
      region->dirty = true;
    }

    blockCount -= regionBlocks;
    buffer     += regionBytes;
    offset      = 0;
  }

  mutex_unlock(&layer->mutex);

  return VDO_SUCCESS;
}

/**
 * Free a RAMLayer.
 *
 * @param layer  The layer to free
 **/
static void freeRAMLayer(RAMLayer *layer)
{
  if (layer == NULL) {
    return;
  }

  if (layer->backing != -1) {
    try_close_file(layer->backing);
  }

  Region *list = uds_forget(layer->regionList);
  while (list != NULL) {
    Region *toFree = list;
    list = list->next;
    uds_free(toFree);
  }

  uds_free(layer->regions);
  mutex_destroy(&layer->mutex);
  uds_free(layer);
}

/**
 * Free a RAMLayer and NULL out the reference to it.
 *
 * Implements layer_destructor.
 *
 * @param layerPtr  A pointer to the layer to free
 **/
static void freeLayer(PhysicalLayer **layerPtr)
{
  PhysicalLayer *layer = *layerPtr;
  if (layer == NULL) {
    return;
  }

  freeRAMLayer(asRAMLayer(layer));
  *layerPtr = NULL;
}

/**********************************************************************/
int makeRAMLayer(block_count_t   blockCount,
                 bool            acceptsFlushes,
                 PhysicalLayer **layerPtr)
{
  RAMLayer *layer;
  int result = uds_allocate(1, RAMLayer, __func__, &layer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  mutex_init(&layer->mutex);
  layer->size        = blockCount * VDO_BLOCK_SIZE;
  layer->regionCount = DIV_ROUND_UP(blockCount, REGION_BLOCKS);
  result = uds_allocate(layer->regionCount,
                        Region *,
                        __func__,
                        &layer->regions);
  if (result != UDS_SUCCESS) {
    freeRAMLayer(layer);
    return result;
  }

  layer->blockCount     = blockCount;
  layer->acceptsFlushes = acceptsFlushes;
  layer->writesEnabled  = true;
  layer->backing        = -1;
  layer->pattern        = INITIAL_RAMLAYER_PATTERN;

  layer->common.destroy             = freeLayer;
  layer->common.getBlockCount       = getBlockCount;
  layer->common.allocateIOBuffer    = allocateIOBuffer;
  layer->common.reader              = ramReader;
  layer->common.writer              = ramWriter;

  *layerPtr = &layer->common;
  return VDO_SUCCESS;
}

/**********************************************************************/
void makeRAMLayerFromFile(const char     *path,
                          bool            acceptsFlushes,
                          PhysicalLayer **layerPtr)
{
  struct stat statbuf;
  VDO_ASSERT_SUCCESS(logging_stat(path, &statbuf, __func__));
  block_count_t blockCount = DIV_ROUND_UP(statbuf.st_size, VDO_BLOCK_SIZE);
  PhysicalLayer *layer;

  VDO_ASSERT_SUCCESS(makeRAMLayer(blockCount, acceptsFlushes, &layer));
  RAMLayer *ramLayer  = asRAMLayer(layer);
  VDO_ASSERT_SUCCESS(open_file(path, FU_READ_ONLY, &ramLayer->backing));
  *layerPtr = layer;
}

/**********************************************************************/
void zeroRAMLayer(PhysicalLayer *layer, physical_block_number_t startBlock, size_t blockCount)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  CU_ASSERT((startBlock + blockCount) <= ramLayer->blockCount);

  RegionNumber regionNumber = startBlock / REGION_BLOCKS;
  physical_block_number_t offset = startBlock % REGION_BLOCKS;
  for (; blockCount > 0; regionNumber++) {
    size_t regionBlocks = min((size_t) (REGION_BLOCKS - offset), (size_t) blockCount);
    size_t regionBytes = regionBlocks * VDO_BLOCK_SIZE;
    Region *region = getRegion(ramLayer, regionNumber, false);
    memset(region->cache + (offset * VDO_BLOCK_SIZE), 0, regionBytes);
    memset(region->data + (offset * VDO_BLOCK_SIZE), 0, regionBytes);

    blockCount -= regionBlocks;
    offset      = 0;
  }
}

/**********************************************************************/
int resizeRAMLayer(PhysicalLayer *header, block_count_t newSize)
{
  RAMLayer *layer      = asRAMLayer(header);
  size_t    newRegions = DIV_ROUND_UP(newSize, REGION_BLOCKS);
  if (newRegions > layer->regionCount) {
    VDO_ASSERT_SUCCESS(uds_reallocate_memory(layer->regions,
                                             (layer->regionCount
                                              * sizeof(Region *)),
                                             newRegions * sizeof(Region *),
                                             __func__,
                                             &layer->regions));
  }

  layer->blockCount  = newSize;
  layer->regionCount = newRegions;
  layer->size        = newSize * VDO_BLOCK_SIZE;
  return VDO_SUCCESS;
}

/**********************************************************************/
void copyRAMLayer(PhysicalLayer *to, PhysicalLayer *from)
{
  RAMLayer *toLayer   = asRAMLayer(to);
  RAMLayer *fromLayer = asRAMLayer(from);
  CU_ASSERT_EQUAL(toLayer->size, fromLayer->size);

  mutex_lock(&toLayer->mutex);
  mutex_lock(&fromLayer->mutex);

  for (RegionNumber r = 0; r < fromLayer->regionCount; r++) {
    Region *from = fromLayer->regions[r];
    if (from == NULL) {
      if (toLayer->regions[r] != NULL) {
        memset(toLayer->regions[r]->cache, fromLayer->pattern, REGION_BYTES);
        memset(toLayer->regions[r]->data, fromLayer->pattern, REGION_BYTES);
      }

      continue;
    }

    // Make both the persisted and cache data of the copy from the persisted
    // data in the source.
    Region *to = getRegion(toLayer, r, false);
    memcpy(to->data, from->data, REGION_BYTES);
    memcpy(to->cache, from->data, REGION_BYTES);
    to->dirty = false;
  }

  mutex_unlock(&fromLayer->mutex);
  mutex_unlock(&toLayer->mutex);
}

/**********************************************************************/
PhysicalLayer *cloneRAMLayer(PhysicalLayer *layer)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  PhysicalLayer *clone;
  VDO_ASSERT_SUCCESS(makeRAMLayer(ramLayer->blockCount,
                                  ramLayer->acceptsFlushes,
                                  &clone));
  copyRAMLayer(clone, layer);
  return clone;
}

/**********************************************************************/
void persistSingleBlockInRAMLayer(PhysicalLayer           *layer,
                                  physical_block_number_t  blockNumber)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  off_t     offset   = (blockNumber % REGION_BLOCKS) * VDO_BLOCK_SIZE;

  mutex_lock(&ramLayer->mutex);
  Region *region = getRegion(ramLayer, blockNumber / REGION_BLOCKS, false);
  if ((region != NULL) && (ramLayer->writesEnabled)) {
    memcpy(region->data + offset, region->cache + offset, VDO_BLOCK_SIZE);
  }
  mutex_unlock(&ramLayer->mutex);
}

/**********************************************************************/
void prepareToCrashRAMLayer(PhysicalLayer *layer)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  mutex_lock(&ramLayer->mutex);
  ramLayer->writesEnabled = false;
  mutex_unlock(&ramLayer->mutex);
}

/**********************************************************************/
void crashRAMLayer(PhysicalLayer *layer)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  mutex_lock(&ramLayer->mutex);
  for (Region *region = ramLayer->regionList;
       region != NULL;
       region = region->next) {
    if (region->dirty) {
      memcpy(region->cache, region->data, REGION_BYTES);
      region->dirty = false;
    }
  }
  ramLayer->writesEnabled = true;
  mutex_unlock(&ramLayer->mutex);
}

/**********************************************************************/
void dumpRAMLayerToFile(PhysicalLayer *layer, int fd)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  mutex_lock(&ramLayer->mutex);
  for (RegionNumber r = 0; r < ramLayer->regionCount; r++) {
    Region *region = ramLayer->regions[r];
    if (region == NULL) {
      continue;
    }

    VDO_ASSERT_SUCCESS(write_buffer_at_offset(fd,
                                              r * REGION_BYTES,
                                              region->data,
                                              REGION_BYTES));
  }
  mutex_unlock(&ramLayer->mutex);
}

/**********************************************************************/
void checkRAMLayerContents(PhysicalLayer   *layer,
                           char            *expectedContents,
                           MismatchChecker  checker)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  block_count_t blocks = ramLayer->blockCount;

  mutex_lock(&ramLayer->mutex);
  for (RegionNumber r = 0; r < ramLayer->regionCount; r++) {
    block_count_t  toCompare = min(blocks, (block_count_t) REGION_BLOCKS);
    Region        *region    = ramLayer->regions[r];
    if (region == NULL) {
      char zero_block[VDO_BLOCK_SIZE];
      memset(zero_block, 0, VDO_BLOCK_SIZE);
      for (block_count_t i = 0; i < toCompare; i++) {
        if (memcmp(zero_block, expectedContents, VDO_BLOCK_SIZE) != 0) {
          checker((r * REGION_BLOCKS) + i, expectedContents, zero_block);
        }
        expectedContents += VDO_BLOCK_SIZE;
      }
    } else {
      char *regionPtr = region->data;
      for (block_count_t i = 0; i < toCompare; i++) {
        if (memcmp(regionPtr, expectedContents, VDO_BLOCK_SIZE) != 0) {
          checker((r * REGION_BLOCKS) + i, expectedContents, regionPtr);
        }
        expectedContents += VDO_BLOCK_SIZE;
        regionPtr        += VDO_BLOCK_SIZE;
      }
    }

    blocks -= REGION_BLOCKS;
  }
  mutex_unlock(&ramLayer->mutex);
}

/**********************************************************************/
void flushRAMLayer(PhysicalLayer *layer)
{
  RAMLayer *ramLayer = asRAMLayer(layer);
  mutex_lock(&ramLayer->mutex);
  if (ramLayer->acceptsFlushes && ramLayer->writesEnabled) {
    for (Region *region = ramLayer->regionList;
         region != NULL;
         region = region->next) {
      if (region->dirty) {
        memcpy(region->data, region->cache, REGION_BYTES);
        region->dirty = false;
      }
    }
  }
  mutex_unlock(&ramLayer->mutex);
}
