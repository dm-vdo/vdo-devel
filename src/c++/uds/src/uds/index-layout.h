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

/*
 * The index layout describes the format of the index on the underlying storage, and is responsible
 * for creating those structures when the index is first created. It also validates the index data
 * when loading a saved index, and updates it when saving the index.
 */
#ifdef TEST_INTERNAL

extern atomic_t saves_begun;
#endif /* TEST_INTERNAL */

struct index_layout;

int __must_check make_uds_index_layout(struct configuration *config,
				       bool new_layout,
				       struct index_layout **layout_ptr);

void free_uds_index_layout(struct index_layout *layout);

int __must_check replace_index_layout_storage(struct index_layout *layout, const char *name);

int __must_check load_index_state(struct index_layout *layout, struct uds_index *index);

int __must_check save_index_state(struct index_layout *layout, struct uds_index *index);

#ifdef TEST_INTERNAL
int __must_check discard_index_state_data(struct index_layout *layout);

#endif /* TEST_INTERNAL */
int __must_check discard_open_chapter(struct index_layout *layout);

u64 __must_check get_uds_volume_nonce(struct index_layout *layout);

int __must_check open_uds_volume_bufio(struct index_layout *layout,
				       size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);

#ifdef TEST_INTERNAL
int update_uds_layout(struct index_layout *layout,
		      struct configuration *config,
		      off_t lvm_offset,
		      off_t offset);

#endif /* TEST_INTERNAL */
#endif /* INDEX_LAYOUT_H */
