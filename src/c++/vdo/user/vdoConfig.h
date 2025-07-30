/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef VDO_CONFIG_H
#define VDO_CONFIG_H

#include "errors.h"

#include "indexer.h"

#include "encodings.h"
#include "types.h"

// The vdo_config structure is fully declared in types.h

/**
 * Initialize the recovery journal state for a new VDO.
 *
 * @return An initialized recovery journal state
 **/
struct recovery_journal_state_7_0 __must_check configureRecoveryJournal(void);

/**
 * Format a physical layer to function as a new VDO. This function must be
 * called on a physical layer before a VDO can be loaded for the first time on a
 * given layer. Once a layer has been formatted, it can be loaded and shut down
 * repeatedly. If a new VDO is desired, this function should be called again.
 *
 * @param config            The configuration parameters for the VDO
 * @param indexConfig       The configuration parameters for the index
 * @param layer             The physical layer the VDO will sit on
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check formatVDO(const struct vdo_config *config,
			   const struct index_config *indexConfig,
			   PhysicalLayer *layer);

/**
 * Calculate minimal VDO based on config parameters.
 *
 * @param config        The configuration parameters for the VDO
 * @param indexConfig   The configuration parameters for the index
 * @param minVDOBlocks  A pointer to hold the minimum blocks needed
 *
 * @return VDO_SUCCESS or error.
 **/
int calculateMinimumVDOFromConfig(const struct vdo_config *config,
				  const struct index_config *indexConfig,
				  block_count_t *minVDOBlocks)
  __attribute__((warn_unused_result));

/**
 * Initialize a layout according to a vdo_config. Exposed for testing only.
 *
 * @param [in]  config          The vdo_config to generate a vdo_layout from
 * @param [in]  startingOffset  The start of the layouts
 * @param [out] layout          The layout to initialize
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
initializeLayoutFromConfig(const struct vdo_config *config,
                           physical_block_number_t  startingOffset,
                           struct layout           *layout);

/**
 * This is a version of formatVDO() which allows the caller to supply the
 * desired VDO nonce and uuid. This function exists to facilitate unit tests
 * which attempt to ensure that version numbers are properly updated when
 * formats change.
 *
 * @param config            The configuration parameters for the VDO
 * @param indexConfig       The configuration parameters for the index
 * @param indexBlocks       Size of the index in blocks
 * @param layer             The physical layer the VDO will sit on
 * @param nonce             The nonce for the VDO
 * @param uuid              The uuid for the VDO
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check formatVDOWithNonce(const struct vdo_config *config,
				    const struct index_config *indexConfig,
				    PhysicalLayer *layer,
				    nonce_t nonce,
				    uuid_t *uuid);

/**
 * Force the VDO to exit read-only mode and rebuild when it next loads
 * by setting the super block state.
 *
 * @param layer  The physical layer on which the VDO resides
 **/
int __must_check forceVDORebuild(PhysicalLayer *layer);

/**
 * Force the VDO to enter read-only mode when off-line.  This is only
 * used by a test utility.
 *
 * @param layer  The physical layer on which the VDO resides
 **/
int __must_check setVDOReadOnlyMode(PhysicalLayer *layer);

#endif /* VDO_CONFIG_H */
