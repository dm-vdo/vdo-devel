/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef UDS_CHAPTER_INDEX_H
#define UDS_CHAPTER_INDEX_H

#include <linux/limits.h>

#include "delta-index.h"
#include "geometry.h"

/*
 * A chapter index for an open chapter is a mutable structure that tracks all the records that have
 * been added to the chapter. A chapter index for a closed chapter is similar except that it is
 * immutable because the contents of a closed chapter can never change, and the immutable structure
 * is more efficient. Both types of chapter index are implemented with a delta index.
 */

enum {
	/* The value returned when no entry is found in the chapter index. */
	NO_CHAPTER_INDEX_ENTRY = U16_MAX,
};

struct open_chapter_index {
	const struct geometry *geometry;
	struct delta_index delta_index;
	u64 virtual_chapter_number;
	u64 volume_nonce;
	size_t memory_size;
};
#ifdef TEST_INTERNAL

/* The number of discards in the open chapter indices. */
extern u64 chapter_index_discard_count;
/* The number of discards used to reset the open chapter indices to empty. */
extern u64 chapter_index_empty_count;
/* The number of overflows in the open chapter indices. */
extern u64 chapter_index_overflow_count;
#endif /* TEST_INTERNAL */

int __must_check uds_make_open_chapter_index(struct open_chapter_index **chapter_index,
					     const struct geometry *geometry,
					     u64 volume_nonce);

void uds_free_open_chapter_index(struct open_chapter_index *chapter_index);

void uds_empty_open_chapter_index(struct open_chapter_index *chapter_index,
				  u64 virtual_chapter_number);

int __must_check uds_put_open_chapter_index_record(struct open_chapter_index *chapter_index,
						   const struct uds_record_name *name,
						   u32 page_number);

int __must_check uds_pack_open_chapter_index_page(struct open_chapter_index *chapter_index,
						  u8 *memory, u32 first_list,
						  bool last_page,
						  u32 *lists_packed);

int __must_check uds_initialize_chapter_index_page(struct delta_index_page *index_page,
						   const struct geometry *geometry,
						   u8 *page_buffer,
						   u64 volume_nonce);

int __must_check uds_validate_chapter_index_page(const struct delta_index_page *index_page,
						 const struct geometry *geometry);

int __must_check uds_search_chapter_index_page(struct delta_index_page *index_page,
					       const struct geometry *geometry,
					       const struct uds_record_name *name,
					       u16 *record_page_ptr);

#endif /* UDS_CHAPTER_INDEX_H */
