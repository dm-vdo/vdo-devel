/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#ifdef TEST_INTERNAL
#include <linux/atomic.h>

#endif /* TEST_INTERNAL */
#include "buffer.h"
#include "config.h"
#include "io-factory.h"
#include "uds.h"
#ifdef TEST_INTERNAL

extern atomic_t saves_begun;
#endif /* TEST_INTERNAL */

enum region_kind {
	RL_KIND_SCRATCH = 0, /* uninitialized or scrapped */
	RL_KIND_HEADER = 1, /* for self-referential items */
	RL_KIND_CONFIG = 100,
	RL_KIND_INDEX = 101,
	RL_KIND_SEAL = 102,
	RL_KIND_VOLUME = 201,
	RL_KIND_SAVE = 202,
	RL_KIND_INDEX_PAGE_MAP = 301,
	RL_KIND_VOLUME_INDEX = 302,
	RL_KIND_OPEN_CHAPTER = 303,
};

struct index_layout;

/**
 * Construct an index layout.
 *
 * @param config      The configuration required for a new layout.
 * @param new_layout  Whether this is a new layout.
 * @param layout_ptr  Where to store the new index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_uds_index_layout(struct configuration *config,
				       bool new_layout,
				       struct index_layout **layout_ptr);

/**
 * Free an index layout.
 *
 * @param layout  The layout to free
 **/
void free_uds_index_layout(struct index_layout *layout);

/**
 * Replace the backing store for the layout.
 *
 * @param layout  The layout
 * @param name    A name describing the new backing store
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check replace_index_layout_storage(struct index_layout *layout,
					      const char *name);

/**
 * Load index state
 *
 * @param layout      The index layout
 * @param index       The index
 *
 * @return            UDS_SUCCESS or error
 **/
int __must_check load_index_state(struct index_layout *layout,
				  struct uds_index *index);

/**
 * Save the current index state, including the open chapter.
 *
 * @param layout        The index layout
 * @param index         The index
 *
 * @return              UDS_SUCCESS or error
 **/
int __must_check save_index_state(struct index_layout *layout,
				  struct uds_index *index);

/**
 * Remove or disable the index state data, for testing.
 *
 * @param layout  The index layout
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note the return value of this function is frequently ignored
 **/
int discard_index_state_data(struct index_layout *layout);

/**
 * Remove the open chater from the most recent save.
 *
 * @param layout  The index layout
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check discard_open_chapter(struct index_layout *layout);

/**
 * Obtain the nonce to be used to store or validate the loading of volume index
 * pages.
 *
 * @param [in]  layout   The index layout.
 *
 * @return The nonce to use.
 **/
uint64_t __must_check get_uds_volume_nonce(struct index_layout *layout);

#ifdef __KERNEL__
/**
 * Obtain a dm_bufio_client for the specified index volume.
 *
 * @param [in]  layout            The index layout.
 * @param [in]  block_size        The size of a volume page
 * @param [in]  reserved_buffers  The count of reserved buffers
 * @param [out] client_ptr        Where to put the new dm_bufio_client
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check open_uds_volume_bufio(struct index_layout *layout,
				       size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);
#else
/**
 * Obtain an IO region for the specified index volume.
 *
 * @param [in]  layout      The index layout.
 * @param [out] region_ptr  Where to put the new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check open_uds_volume_region(struct index_layout *layout,
					struct io_region **region_ptr);
#endif

#ifdef TEST_INTERNAL
/**
 * Update and write out an index layout and configuration with a block offset
 *
 * @param layout      The index_layout to be reconfigured
 * @param config      The configuration to be written with the layout
 * @param lvm_offset  The adjustment for lvm space, in bytes
 * @param offset      The offset in bytes to move the index
 *
 * @return  UDS_SUCCESS or a error code
 */
int update_uds_layout(struct index_layout *layout,
		      struct configuration *config,
		      off_t lvm_offset,
		      off_t offset);

#endif /* TEST_INTERNAL */
#endif /* INDEX_LAYOUT_H */
