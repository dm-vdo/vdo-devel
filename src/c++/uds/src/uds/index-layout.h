/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef UDS_INDEX_LAYOUT_H
#define UDS_INDEX_LAYOUT_H

#ifdef TEST_INTERNAL
#include <linux/atomic.h>

#endif /* TEST_INTERNAL */
#include "config.h"
#include "io-factory.h"
#include "uds.h"

/*
 * The index layout describes the format of the index on the underlying storage, and is responsible
 * for creating those structures when the index is first created. It also validates the index data
 * when loading a saved index, and updates it when saving the index.
 */
#ifdef TEST_INTERNAL

extern atomic_t saves_begun;
#endif /* TEST_INTERNAL */

struct index_layout;

int __must_check uds_make_index_layout(struct uds_configuration *config, bool new_layout,
				       struct index_layout **layout_ptr);

void uds_free_index_layout(struct index_layout *layout);

int __must_check uds_replace_index_layout_storage(struct index_layout *layout,
						  struct block_device *bdev);

int __must_check uds_load_index_state(struct index_layout *layout,
				      struct uds_index *index);

int __must_check uds_save_index_state(struct index_layout *layout,
				      struct uds_index *index);

#ifdef TEST_INTERNAL
int __must_check discard_index_state_data(struct index_layout *layout);

#endif /* TEST_INTERNAL */
int __must_check uds_discard_open_chapter(struct index_layout *layout);

u64 __must_check uds_get_volume_nonce(struct index_layout *layout);

int __must_check uds_open_volume_bufio(struct index_layout *layout, size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);

#ifdef TEST_INTERNAL
int update_uds_layout(struct index_layout *layout, struct uds_configuration *config,
		      off_t lvm_offset, off_t offset);

#endif /* TEST_INTERNAL */
#endif /* UDS_INDEX_LAYOUT_H */
