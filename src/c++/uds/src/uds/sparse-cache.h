/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SPARSE_CACHE_H
#define SPARSE_CACHE_H

#include "geometry.h"
#include "type-defs.h"

#ifdef TEST_INTERNAL
/* Basic counts of hits and misses for a given type of cache probe. */
struct cache_counts_by_kind {
	uint64_t hits;
	uint64_t misses;
	uint64_t queued;
};

struct cache_counters {
	/* Number of cache entry invalidations due to single-entry eviction */
	uint64_t evictions;
	/* Number of cache entry invalidations due to chapter expiration */
	uint64_t expirations;
	/* Hit/miss counts for the sparse cache chapter probes */
	struct cache_counts_by_kind sparse_chapters;
	/* Hit/miss counts for the sparce cache name searches */
	struct cache_counts_by_kind sparse_searches;
};

#endif /* TEST_INTERNAL */
struct index_zone;
struct sparse_cache;

int __must_check make_sparse_cache(const struct geometry *geometry,
				   unsigned int capacity,
				   unsigned int zone_count,
				   struct sparse_cache **cache_ptr);

void free_sparse_cache(struct sparse_cache *cache);

size_t get_sparse_cache_memory_size(const struct sparse_cache *cache);

#ifdef TEST_INTERNAL
struct cache_counters
get_sparse_cache_counters(const struct sparse_cache *cache);

#endif /* TEST_INTERNAL */
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
