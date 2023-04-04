/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_PAGE_MAP_H
#define INDEX_PAGE_MAP_H 1

#include "geometry.h"
#include "io-factory.h"

/*
 * The index maintains a page map which records how the chapter delta lists are distributed among
 * the index pages for each chapter, allowing the volume to be efficient about reading only pages
 * that it knows it will need.
 */

struct index_page_map {
	const struct geometry *geometry;
	u64 last_update;
	u32 entries_per_chapter;
	u16 *entries;
};

int __must_check make_index_page_map(const struct geometry *geometry,
				     struct index_page_map **map_ptr);

void free_index_page_map(struct index_page_map *map);

int __must_check read_index_page_map(struct index_page_map *map, struct buffered_reader *reader);

int __must_check write_index_page_map(struct index_page_map *map, struct buffered_writer *writer);

void update_index_page_map(struct index_page_map *map,
			   u64 virtual_chapter_number,
			   u32 chapter_number,
			   u32 index_page_number,
			   u32 delta_list_number);

u32 __must_check find_index_page_number(const struct index_page_map *map,
					const struct uds_record_name *name,
					u32 chapter_number);

void get_list_number_bounds(const struct index_page_map *map,
			    u32 chapter_number,
			    u32 index_page_number,
			    u32 *lowest_list,
			    u32 *highest_list);

u64 compute_index_page_map_save_size(const struct geometry *geometry);

#endif /* INDEX_PAGE_MAP_H */
