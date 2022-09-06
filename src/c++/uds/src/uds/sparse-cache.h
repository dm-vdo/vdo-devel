/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SPARSE_CACHE_H
#define SPARSE_CACHE_H

#include "geometry.h"
#include "type-defs.h"
#include "uds.h"

struct index_zone;
struct sparse_cache;

int __must_check make_sparse_cache(const struct geometry *geometry,
				   unsigned int capacity,
				   unsigned int zone_count,
				   struct sparse_cache **cache_ptr);

void free_sparse_cache(struct sparse_cache *cache);

size_t get_sparse_cache_memory_size(const struct sparse_cache *cache);

bool sparse_cache_contains(struct sparse_cache *cache,
			   uint64_t virtual_chapter,
			   unsigned int zone_number);

int __must_check update_sparse_cache(struct index_zone *zone,
				     uint64_t virtual_chapter);

void invalidate_sparse_cache(struct sparse_cache *cache);

int __must_check search_sparse_cache(struct index_zone *zone,
				     const struct uds_chunk_name *name,
				     uint64_t *virtual_chapter_ptr,
				     int *record_page_ptr);

#endif /* SPARSE_CACHE_H */
