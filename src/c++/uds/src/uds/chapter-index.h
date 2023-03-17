/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef CHAPTER_INDEX_H
#define CHAPTER_INDEX_H 1

#include "delta-index.h"
#include "geometry.h"

/*
 * A chapter index for an open chapter is a mutable structure that tracks all the records that have
 * been added to the chapter. A chapter index for a closed chapter is similar except that it is
 * immutable because the contents of a closed chapter can never change, and the immutable structure
 * is more efficient. Both types of chapter index are implemented with a delta index.
 */

enum {
	/*
	 * The value returned as the record page number when an entry is not found in the chapter
	 * index.
	 */
	NO_CHAPTER_INDEX_ENTRY = -1
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
extern long chapter_index_discard_count;
/* The number of discards used to reset the open chapter indices to empty. */
extern long chapter_index_empty_count;
/* The number of overflows in the open chapter indices. */
extern long chapter_index_overflow_count;
#endif /* TEST_INTERNAL */

int __must_check make_open_chapter_index(struct open_chapter_index **chapter_index,
					 const struct geometry *geometry,
					 u64 volume_nonce);

void free_open_chapter_index(struct open_chapter_index *chapter_index);

void empty_open_chapter_index(struct open_chapter_index *chapter_index,
			      u64 virtual_chapter_number);

int __must_check put_open_chapter_index_record(struct open_chapter_index *chapter_index,
					       const struct uds_record_name *name,
					       unsigned int page_number);

int __must_check pack_open_chapter_index_page(struct open_chapter_index *chapter_index,
					      u8 *memory,
					      unsigned int first_list,
					      bool last_page,
					      unsigned int *num_lists);

int __must_check initialize_chapter_index_page(struct delta_index_page *index_page,
					       const struct geometry *geometry,
					       u8 *page_buffer,
					       u64 volume_nonce);

int __must_check validate_chapter_index_page(const struct delta_index_page *index_page,
					     const struct geometry *geometry);

int __must_check search_chapter_index_page(struct delta_index_page *index_page,
					   const struct geometry *geometry,
					   const struct uds_record_name *name,
					   int *record_page_ptr);

#endif /* CHAPTER_INDEX_H */
