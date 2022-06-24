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
	RL_KIND_HEADER = 1,
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

int __must_check make_uds_index_layout(struct configuration *config,
				       bool new_layout,
				       struct index_layout **layout_ptr);

void free_uds_index_layout(struct index_layout *layout);

int __must_check replace_index_layout_storage(struct index_layout *layout,
					      const char *name);

int __must_check load_index_state(struct index_layout *layout,
				  struct uds_index *index);

int __must_check save_index_state(struct index_layout *layout,
				  struct uds_index *index);

int discard_index_state_data(struct index_layout *layout);

int __must_check discard_open_chapter(struct index_layout *layout);

uint64_t __must_check get_uds_volume_nonce(struct index_layout *layout);

#ifdef __KERNEL__
int __must_check open_uds_volume_bufio(struct index_layout *layout,
				       size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);
#else
int __must_check open_uds_volume_region(struct index_layout *layout,
					struct io_region **region_ptr);
#endif

#ifdef TEST_INTERNAL
int update_uds_layout(struct index_layout *layout,
		      struct configuration *config,
		      off_t lvm_offset,
		      off_t offset);

#endif /* TEST_INTERNAL */
#endif /* INDEX_LAYOUT_H */
