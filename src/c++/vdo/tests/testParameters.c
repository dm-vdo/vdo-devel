/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testParameters.h"

#include "memory-alloc.h"

#include "block-map.h"
#include "constants.h"
#include "device-config.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "thread-config.h"
#include "types.h"
#include "vdo-component-states.h"
#include "volume-geometry.h"

#include "dataBlocks.h"
#include "vdoAsserts.h"

enum {
  MAX_DEFAULT_DATA_BLOCKS = 8 * 1024,
  VDO_LAYOUT_START        = 2, // Covers geometry, index, and super blocks.
};

static const TestParameters DEFAULT_PARAMETERS = {
  .physicalBlocks       = 0,    // computed (256 + 1 + 6 + 1 + 1 + 4)
  .logicalBlocks        = 0,    // computed (512)
  .mappableBlocks       = 256,
  .cacheSize            = 4,
  .blockMapMaximumAge   = 0,    // computed (2)
  .slabSize             = 0,    // computed (16)
  .slabCount            = 16,
  .slabJournalBlocks    = 2,
  .journalBlocks        = 4,
  .logicalThreadCount   = 0,
  .physicalThreadCount  = 0,
  .hashZoneThreadCount  = 0,
  .synchronousStorage   = false,
  .dataFormatter        = fillWithOffset,
  .enableCompression    = false,
  .disableDeduplication = false,
  .noIndexRegion        = false,
  .backingFile          = NULL,
};

static char *DEVICE_NAME = "test device name";

/**
 * Apply default values for any unspecified parameters whose values are not
 * computed.
 *
 * @param parameters  The supplied parameters
 *
 * @return The supplied parameters with unspecified values replaced by
 *         default values
 **/
static TestParameters applyDefaults(const TestParameters *parameters)
{
  if (parameters == NULL) {
    return DEFAULT_PARAMETERS;
  }

  TestParameters applied = DEFAULT_PARAMETERS;
  if (parameters->physicalBlocks != 0) {
    applied.physicalBlocks = parameters->physicalBlocks;
  }

  if (parameters->logicalBlocks != 0) {
    applied.logicalBlocks = parameters->logicalBlocks;
  }

  if (parameters->mappableBlocks != 0) {
    applied.mappableBlocks = parameters->mappableBlocks;
  }

  if (parameters->cacheSize != 0) {
    applied.cacheSize = parameters->cacheSize;
  }

  if (parameters->blockMapMaximumAge != 0) {
    applied.blockMapMaximumAge = parameters->blockMapMaximumAge;
  }

  // If slab size is specified, don't default the slab count
  if (parameters->slabSize != 0) {
    applied.slabSize  = parameters->slabSize;
    // Don't use the default for the slab count if slab size was specified.
    applied.slabCount = 0;
  }

  if (parameters->slabCount != 0) {
    applied.slabCount = parameters->slabCount;
  }

  if (parameters->slabJournalBlocks != 0) {
    applied.slabJournalBlocks = parameters->slabJournalBlocks;
  }

  if (parameters->journalBlocks != 0) {
    applied.journalBlocks = parameters->journalBlocks;
  }

  if (parameters->logicalThreadCount != 0) {
    applied.logicalThreadCount = parameters->logicalThreadCount;
  }

  if (parameters->physicalThreadCount != 0) {
    applied.physicalThreadCount = parameters->physicalThreadCount;
  }

  if (parameters->hashZoneThreadCount != 0) {
    applied.hashZoneThreadCount = parameters->hashZoneThreadCount;
  }

  if (parameters->dataFormatter != NULL) {
    applied.dataFormatter = parameters->dataFormatter;
  }

  if (parameters->enableCompression != applied.enableCompression) {
    applied.enableCompression = parameters->enableCompression;
  }

  if (parameters->disableDeduplication != applied.disableDeduplication) {
    applied.disableDeduplication = parameters->disableDeduplication;
  }

  if (parameters->synchronousStorage) {
    applied.synchronousStorage = true;
  }

  if (parameters->noIndexRegion) {
    applied.noIndexRegion = true;
    // We can't have dedupe if we don't have an index, and startAsyncLayer()
    // will hang if it expects there to be an index when there won't be.
    applied.disableDeduplication = true;
  }

  if (parameters->backingFile) {
    applied.backingFile = parameters->backingFile;
  }

  return applied;
}

/**
 * Compute the number of logical blocks to configure.
 *
 * @param logicalBlocks   The requested number of logical blocks
 * @param mappableBlocks  The number of mappable blocks in the VDO
 *
 * @return The number of logical blocks
 **/
static block_count_t determineLogicalBlocks(block_count_t logicalBlocks,
                                            block_count_t mappableBlocks)
{
  return ((logicalBlocks == 0) ? 2 * mappableBlocks : logicalBlocks);
}

/**
 * Compute the number of physical blocks to configure.
 *
 * @param parameters  The parameters which have been configured so far
 **/
static void computePhysicalBlocks(TestParameters *parameters)
{
  parameters->physicalBlocks = ((parameters->slabCount * parameters->slabSize)
                                + VDO_LAYOUT_START + parameters->journalBlocks
                                + DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT
                                + VDO_SLAB_SUMMARY_BLOCKS);
}

/**
 * Check whether the physical size should be computed from the specified slab
 * size and slab count.
 *
 * @param parameters  The parameters from the test
 **/
static bool physicalFromSlabCount(const TestParameters *parameters)
{
  return ((parameters != NULL)
          && (parameters->physicalBlocks == 0)
          && (parameters->slabSize > 0)
          && (parameters->slabCount > 0)
          && (parameters->mappableBlocks == 0));
}

/**
 * Compute a complete set of test parameters.
 *
 * @param parameters  The supplied test parameters
 *
 * @return A complete set of parameters with unspecified values computed
 **/
static TestParameters computeParameters(const TestParameters *parameters)
{
  TestParameters params = applyDefaults(parameters);

  // Default the era length to half of the journal size.
  if (params.blockMapMaximumAge == 0) {
    params.blockMapMaximumAge
      = vdo_get_recovery_journal_length(params.journalBlocks) / 2;
  }

  // Build an initial slab config.
  struct slab_config slabConfig;
  if (params.slabSize == 0) {
    // Slab size must also be specified if the physical size is specified.
    if (params.physicalBlocks > 0) {
      CU_FAIL("Must specify slab size when specifying physical blocks");
    }

    if (params.slabCount == 0) {
      CU_FAIL("Must specify slab size or slab count");
    }

    /**
     * Try increasing the slab size until it meets the minimum space
     * requirement for the requested number of mappable blocks. This for the
     * block allocator and the block map. This will be adjusted later to
     * account for block map overhead. Data blocks per slab must be at least 2
     * so that the unopened slab priority calculation in block allocator works.
     **/
    params.slabSize = 1;
    int result;
    do {
      // Assumes there is some power of two slab size that will actually work.
      params.slabSize <<= 1;
      result = vdo_configure_slab(params.slabSize, params.slabJournalBlocks,
                                  &slabConfig);
    } while ((result != VDO_SUCCESS)
             || ((slabConfig.data_blocks * params.slabCount)
                 < params.mappableBlocks)
             || (slabConfig.data_blocks == 1));
    if (physicalFromSlabCount(parameters)) {
      computePhysicalBlocks(&params);
    }
  } else {
    VDO_ASSERT_SUCCESS(vdo_configure_slab(params.slabSize,
                                          params.slabJournalBlocks,
                                          &slabConfig));
    if (physicalFromSlabCount(parameters)) {
      computePhysicalBlocks(&params);
    }
  }

  // Determine physical, logical, and mappable sizes.
  if (params.physicalBlocks > 0) {
    // This is like production: the total physical capacity is specified.
    // Measure the size of the block map and derive the mappable size.
    block_count_t overhead = (VDO_LAYOUT_START + params.journalBlocks
                              + VDO_SLAB_SUMMARY_BLOCKS
                              + DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
    if ((overhead + params.slabSize) <= params.physicalBlocks) {
      params.slabCount = DIV_ROUND_UP(params.physicalBlocks - overhead,
                                      params.slabSize);
    } else {
      params.slabCount = 1;
    }
    params.logicalBlocks = determineLogicalBlocks(params.logicalBlocks,
                                                  (params.slabCount
                                                   * slabConfig.data_blocks));
  } else if (params.mappableBlocks == 0) {
    CU_FAIL("Must specify physicalBlocks or mappableBlocks");
  } else {
    // We need to derive the physical size for tests that only want to specify
    // a minimum number of mappable data blocks.
    block_count_t overhead = 0;
    block_count_t attempt  = params.mappableBlocks;
    for (block_count_t mappable = 0; mappable < params.mappableBlocks;
         attempt += overhead) {
      params.slabCount     = DIV_ROUND_UP(attempt, slabConfig.data_blocks);
      attempt              = params.slabCount * slabConfig.data_blocks;
      params.logicalBlocks = determineLogicalBlocks(params.logicalBlocks,
                                                    attempt);
      overhead             = computeBlockMapOverhead(params.logicalBlocks);
      if (overhead < attempt) {
        mappable = attempt - overhead;
      }
    }

    computePhysicalBlocks(&params);
  }

  return params;
}

/**********************************************************************/
TestConfiguration makeTestConfiguration(const TestParameters *parameters)
{
  TestParameters params = computeParameters(parameters);
  if ((params.logicalThreadCount + params.physicalThreadCount
       + params.hashZoneThreadCount) > 0) {
    if (params.logicalThreadCount == 0) {
      params.logicalThreadCount = 1;
    }

    if (params.physicalThreadCount == 0) {
      params.physicalThreadCount = 1;
    }

    if (params.hashZoneThreadCount == 0) {
      params.hashZoneThreadCount = 1;
    }
  }

  struct index_config indexConfig;
  block_count_t indexBlocks;
  if (params.noIndexRegion) {
    indexConfig = (struct index_config) {
      .mem    = 0,
      .sparse = false,
    };
    indexBlocks = 0;
  } else {
    indexConfig = (struct index_config) {
      .mem    = UDS_MEMORY_CONFIG_TINY_TEST,
      .sparse = false,
    };
    VDO_ASSERT_SUCCESS(vdo_compute_index_blocks(&indexConfig, &indexBlocks));
  }

  TestConfiguration configuration = (TestConfiguration) {
    .config             = (struct vdo_config) {
      .logical_blocks        = params.logicalBlocks,
      .physical_blocks       = params.physicalBlocks + indexBlocks,
      .slab_size             = params.slabSize,
      .slab_journal_blocks   = params.slabJournalBlocks,
      .recovery_journal_size = params.journalBlocks,
    },
    .deviceConfig       = (struct device_config) {
      .cache_size            = params.cacheSize,
      .block_map_maximum_age = params.blockMapMaximumAge,
      .thread_counts         = (struct thread_count_config) {
        .logical_zones         = params.logicalThreadCount,
        .physical_zones        = params.physicalThreadCount,
        .hash_zones            = params.hashZoneThreadCount,
        .bio_threads           = DEFAULT_VDO_BIO_SUBMIT_QUEUE_COUNT,
        .bio_rotation_interval = DEFAULT_VDO_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
        .bio_ack_threads       = 1,
        .cpu_threads           = 1,
      },
      .max_discard_blocks = 1500,
      .parent_device_name = DEVICE_NAME,
      .logical_blocks     = params.logicalBlocks,
      .logical_block_size = VDO_BLOCK_SIZE,
      .physical_blocks    = params.physicalBlocks + indexBlocks,
      .compression        = params.enableCompression,
      .deduplication      = !params.disableDeduplication,
    },
    .indexConfig         = indexConfig,
    .indexRegionStart    = 1,
    .vdoRegionStart      = indexBlocks + 1,
    .synchronousStorage  = params.synchronousStorage,
    .dataFormatter       = params.dataFormatter,
    .backingFile         = params.backingFile,
  };

  if ((parameters == NULL) || (parameters->modifier == NULL)) {
    return configuration;
  }

  return parameters->modifier(configuration);
}

/**********************************************************************/
block_count_t computeBlockMapOverhead(block_count_t logicalBlocks)
{
  page_count_t pages = DIV_ROUND_UP(logicalBlocks,
                                    VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
  if (pages <= DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT) {
    return pages * (VDO_BLOCK_MAP_TREE_HEIGHT - 1);
  }

  page_count_t pagesPerRoot = pages / DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT;
  root_count_t extra = pages - (pagesPerRoot
                                * DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  block_count_t overhead = pages;
  page_count_t pagesPerFullerRoot = pagesPerRoot + 1;
  for (height_t h = 1; h < VDO_BLOCK_MAP_TREE_HEIGHT - 1; h++) {
    pagesPerRoot = DIV_ROUND_UP(pagesPerRoot, VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
    pagesPerFullerRoot = DIV_ROUND_UP(pagesPerFullerRoot,
                                      VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
    overhead += ((pagesPerRoot
                 * (DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT - extra))
                 + (pagesPerFullerRoot * extra));
  }

  return overhead;
}

/**********************************************************************/
struct thread_config *makeOneThreadConfig(void)
{
  // match the defaults from vdo_parse_device_config
  struct thread_count_config counts = {
    .bio_ack_threads = 1,
    .bio_threads = DEFAULT_VDO_BIO_SUBMIT_QUEUE_COUNT,
    .bio_rotation_interval = DEFAULT_VDO_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL,
    .cpu_threads = 1,
  };

  struct thread_config *config;
  VDO_ASSERT_SUCCESS(vdo_make_thread_config(counts, &config));
  return config;
}




