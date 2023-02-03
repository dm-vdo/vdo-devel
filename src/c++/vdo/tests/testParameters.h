/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_PARAMETERS_H
#define TEST_PARAMETERS_H

#include "device-config.h"
#include "thread-config.h"
#include "types.h"
#include "vdo-component.h"
#include "volume-geometry.h"

#include "dataBlocks.h"

typedef struct testConfiguration TestConfiguration;

/**
 * A function to modify a test configuration after the it has been generated
 * from the test parameters. This is may be used for very specific
 * customization.
 **/
typedef TestConfiguration ConfigurationModifier(TestConfiguration config);

/**
 * The set of parameters used to configure unit tests. The parameters are used
 * to derive a TestConfiguration.
 *
 * Default values are computed for any field which is not specified.
 **/
typedef struct testParameters {
  /** How many physical blocks */
  block_count_t             physicalBlocks;
  /** How many logical blocks */
  block_count_t             logicalBlocks;
  /** How many usable physical blocks */
  block_count_t             mappableBlocks;
  /** How many cache pages */
  page_count_t              cacheSize;
  /** How fast to write dirty pages out */
  block_count_t             blockMapMaximumAge;
  /** How many block entries per slab */
  block_count_t             slabSize;
  /** How many slabs */
  slab_count_t              slabCount;
  /** How big is each slab journal */
  block_count_t             slabJournalBlocks;
  /** How big is the recovery journal */
  block_count_t             journalBlocks;
  /** The number of logical threads */
  thread_count_t            logicalThreadCount;
  /** The number of physical threads */
  thread_count_t            physicalThreadCount;
  /** The number of hash zone threads */
  thread_count_t            hashZoneThreadCount;
  /** Whether the underlying storage is synchronous */
  bool                      synchronousStorage;
  /** A function to modify the config generated from these parameters */
  ConfigurationModifier    *modifier;

  // vdoTestBase parameters
  /** The formatter for test data blocks */
  DataFormatter            *dataFormatter;
  /** Whether compression should be enabled */
  bool                      enableCompression;
  /** Whether deduplication should be enabled */
  bool                      disableDeduplication;
  /** Whether physicalBlocks should include an index region */
  bool                      noIndexRegion;
  /** The backing file from which to initially load the RAMLayer (if not NULL) */
  const char               *backingFile;
} TestParameters;

/**
 * The configuration for a test, derived from TestParameters.
 **/
struct testConfiguration {
  struct vdo_config        config;
  struct device_config     deviceConfig;
  struct index_config      indexConfig;
  physical_block_number_t  indexRegionStart;
  physical_block_number_t  vdoRegionStart;
  bool                     synchronousStorage;
  DataFormatter           *dataFormatter;
  const char              *backingFile;
};

/**
 * Compute a test configuration.
 *
 * @param parameters  the test parameters, which may be partially filled in
 *                    with uninteresting values defaulted to 0
 *
 * @return A test configuration based on the supplied parameters; the caller is
 *         responsible for freeing the ThreadConfig in the returned
 *         configuration
 **/
TestConfiguration makeTestConfiguration(const TestParameters *parameters)
  __attribute__((warn_unused_result));

/**
 * Build a slab_config from a set of test parameters.
 *
 * @return a fully populated struct slab_config structure
 **/
struct slab_config getSlabConfigFromParameters(TestParameters parameters);

/**
 * Compute the number of block map pages which will be allocated as the result
 * of writing the specified number of contiguous logical blocks, starting from
 * the first LBN in the arboreal portion of the block map.
 *
 * @param logicalBlocks  The number of logical blocks to be written
 *
 * @return The resulting block map overhead
 **/
block_count_t computeBlockMapOverhead(block_count_t logicalBlocks);

/**
 * Make a one thread config, will ASSERT on error.
 *
 * @return The new config
 **/
struct thread_config *makeOneThreadConfig(void);

#endif // TEST_PARAMETERS_H
