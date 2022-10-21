/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SPARSE_CACHE_H
#define SPARSE_CACHE_H

#include "geometry.h"
#include "type-defs.h"
#include "uds.h"

/*
 * The sparse cache is a cache of entire chapter indexes from sparse chapters
 * used for searching for names after all other search paths have failed. It
 * contains only complete chapter indexes; record pages from sparse chapters
 * and single index pages used for resolving hooks are kept in the regular page
 * cache in the volume.
 *
 * The most important property of this cache is the absence of synchronization
 * for read operations. Safe concurrent access to the cache by the zone
 * threads is controlled by the triage queue and the barrier requests it
 * issues to the zone queues. The set of cached chapters does not and must not
 * change between the carefully coordinated calls to update_sparse_cache() from
 * the zone threads. Outside of updates, every zone will get the same result
 * when calling sparse_cache_contains() as every other zone.
 */

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
				     const struct uds_record_name *name,
				     uint64_t *virtual_chapter_ptr,
				     int *record_page_ptr);

#endif /* SPARSE_CACHE_H */
