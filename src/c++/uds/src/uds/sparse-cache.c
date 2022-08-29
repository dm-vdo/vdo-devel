// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "sparse-cache.h"

#include <linux/dm-bufio.h>

#include "chapter-index.h"
#include "common.h"
#include "config.h"
#include "index.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds-threads.h"

/*
 * The sparse cache is a cache of entire chapter indexes from sparse chapters
 * used for searching for names after all other search paths have failed. It
 * contains only complete chapter indexes; record pages from sparse chapters
 * and single index pages used for resolving hooks are kept in the regular page
 * cache.
 *
 * The most important property of this cache is the absence of synchronization
 * for read operations. Safe concurrent access to the cache by the zone
 * threads is controlled by the triage queue and the barrier requests it
 * issues to the zone queues. The set of cached chapters does not and must not
 * change between the carefully coordinated calls to update_sparse_cache() from
 * the zone threads.
 *
 * Since the cache is small, it is implemented as a simple array of cache
 * entries. Searching for a specific virtual chapter is implemented as a linear
 * search. The cache replacement policy is least-recently-used (LRU). Again,
 * the small size of the cache allows the LRU order to be maintained by
 * shifting entries in an array list.
 *
 * Changing the contents of the cache requires the coordinated participation of
 * all zone threads via the careful use of barrier messages sent to all the
 * index zones by the triage queue worker thread. The critical invariant for
 * coordination is that the cache membership must not change between updates,
 * so that all calls to sparse_cache_contains() from the zone threads must all
 * receive the same results for every virtual chapter number. To ensure that
 * critical invariant, state changes such as "that virtual chapter is no longer
 * in the volume" and "skip searching that chapter because it has had too many
 * cache misses" are represented separately from the cache membership
 * information (the virtual chapter number).
 *
 * As a result of this invariant, we have the guarantee that every zone thread
 * will call update_sparse_cache() once and exactly once to request a chapter
 * that is not in the cache, and the serialization of the barrier requests from
 * the triage queue ensures they will all request the same chapter number. This
 * means the only synchronization we need can be provided by a pair of thread
 * barriers used only in the update_sparse_cache() call, providing a critical
 * section where a single zone thread can drive the cache update while all the
 * other zone threads are known to be blocked, waiting in the second barrier.
 * Outside that critical section, all the zone threads implicitly hold a shared
 * lock. Inside it, the thread for zone zero holds an exclusive lock. No other
 * threads may access or modify the cache entries.
 *
 * Cache statistics must only be modified by a single thread, which is also the
 * zone zero thread. All fields that might be frequently updated by that thread
 * are kept in separate cache-aligned structures so they will not cause cache
 * contention via "false sharing" with the fields that are frequently accessed
 * by all of the zone threads.
 *
 * The LRU order is managed independently by each zone thread, and each zone
 * uses its own list for searching and cache membership queries. The zone zero
 * list is used to decide which chapter to evict when the cache is updated, and
 * its search list is copied to the other threads at that time.
 *
 * The virtual chapter number field of the cache entry is the single field
 * indicating whether a chapter is a member of the cache or not. The value
 * UINT64_MAX is used to represent a null or undefined chapter number. When
 * present in the virtual chapter number field of a cached_chapter_index, it
 * indicates that the cache entry is dead, and all the other fields of that
 * entry (other than immutable pointers to cache memory) are undefined and
 * irrelevant. Any cache entry that is not marked as dead is fully defined and
 * a member of the cache, and sparse_cache_contains() will always return true
 * for any virtual chapter number that appears in any of the cache entries.
 *
 * A chapter index that is a member of the cache may be excluded from searches
 * between calls to update_sparse_cache() in two different ways. First, when a
 * chapter falls off the end of the volume, its virtual chapter number will be
 * less that the oldest virtual chapter number. Since that chapter is no longer
 * part of the volume, there's no point in continuing to search that chapter
 * index. Once invalidated, that virtual chapter will still be considered a
 * member of the cache, but it will no longer be searched for matching names.
 *
 * The second mechanism is a heuristic based on keeping track of the number of
 * consecutive search misses in a given chapter index. Once that count exceeds
 * a threshold, the skip_search flag will be set to true, causing the chapter
 * to be skipped when searching the entire cache, but still allowing it to be
 * found when searching for a hook in that specific chapter. Finding a hook
 * will clear the skip_search flag, once again allowing the non-hook searches
 * to use that cache entry. Again, regardless of the state of the skip_search
 * flag, the virtual chapter must still considered to be a member of the cache
 * for sparse_cache_contains().
 */

enum {
	SKIP_SEARCH_THRESHOLD = 20000,
	ZONE_ZERO = 0,
};

/*
 * These counters are essentially fields of the struct cached_chapter_index,
 * but are segregated into this structure because they are frequently modified.
 * They are grouped and aligned to keep them on different cache lines from the
 * chapter fields that are accessed far more often than they are updated.
 */
struct __attribute__((aligned(CACHE_LINE_BYTES))) cached_index_counters {
	uint64_t search_hits;
	uint64_t search_misses;
	uint64_t consecutive_misses;
};

struct __attribute__((aligned(CACHE_LINE_BYTES))) cached_chapter_index {
	/*
	 * The virtual chapter number of the cached chapter index. UINT64_MAX
	 * means this cache entry is unused. This field must only be modified
	 * in the critical section in update_sparse_cache().
	 */
	uint64_t virtual_chapter;

	unsigned int index_pages_count;

	/*
	 * If set, skip the chapter when searching the entire cache. This flag
	 * is just a performance optimization. This flag is mutable between
	 * cache updates, but it rarely changes and is frequently accessed, so
	 * it groups with the immutable fields.
	 */
	bool skip_search;

	/*
	 * These pointers are immutable during the life of the cache. The
	 * contents of the arrays change when the cache entry is replaced.
	 */
	struct delta_index_page *index_pages;
	struct dm_buffer **volume_buffers;

	/*
	 * The cache-aligned counters change often and are placed at the end of
	 * the structure to prevent false sharing with the more stable fields
	 * above.
	 */
	struct cached_index_counters counters;
};

/*
 * A search_list represents an ordering of the sparse chapter index cache entry
 * array, from most recently accessed to least recently accessed, which is the
 * order in which the indexes should be searched and the reverse order in which
 * they should be evicted from the cache.
 *
 * Cache entries that are dead or empty are kept at the end of the list,
 * avoiding the need to even iterate over them to search, and ensuring that
 * dead entries are replaced before any live entries are evicted.
 *
 * The search list is instantated for each zone thread, avoiding any need for
 * synchronization. The structure is allocated on a cache boundary to avoid
 * false sharing of memory cache lines between zone threads.
 */
struct search_list {
	uint8_t capacity;
	uint8_t first_dead_entry;
	uint8_t entries[];
};

struct search_list_iterator {
	struct search_list *list;
	unsigned int next_entry;
	struct cached_chapter_index *chapters;
};

/*
 * These counter values are essentially fields of the sparse_cache, but are
 * segregated into this structure because they are frequently modified. We
 * group them and align them to keep them on different cache lines from the
 * cache fields that are accessed far more often than they are updated.
 */
struct sparse_cache_counters {
	uint64_t chapter_hits;
	uint64_t chapter_misses;
	uint64_t search_hits;
	uint64_t search_misses;
	uint64_t invalidations;
	uint64_t evictions;
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct sparse_cache {
	unsigned int capacity;
	unsigned int zone_count;
	const struct geometry *geometry;

	unsigned int skip_search_threshold;
	struct search_list *search_lists[MAX_ZONES];

	struct barrier begin_cache_update;
	struct barrier end_cache_update;

	struct sparse_cache_counters counters;
	struct cached_chapter_index chapters[];
};

static int __must_check
initialize_cached_chapter_index(struct cached_chapter_index *chapter,
				const struct geometry *geometry)
{
	int result;

	chapter->virtual_chapter = UINT64_MAX;
	chapter->index_pages_count = geometry->index_pages_per_chapter;

	result = UDS_ALLOCATE(chapter->index_pages_count,
			      struct delta_index_page,
			      __func__,
			      &chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_ALLOCATE(chapter->index_pages_count,
			    struct dm_buffer *,
			    "sparse index volume pages",
			    &chapter->volume_buffers);
}

static int __must_check make_search_list(unsigned int capacity,
					 struct search_list **list_ptr)
{
	struct search_list *list;
	unsigned int bytes;
	uint8_t i;
	int result;

	if (capacity == 0) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list must have entries");
	}
	if (capacity > UINT8_MAX) {
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "search list capacity must fit in 8 bits");
	}

	/*
	 * We need three temporary entry arrays for purge_search_list().
	 * Allocate them contiguously with the main array.
	 */
	bytes = sizeof(struct search_list) + (4 * capacity * sizeof(uint8_t));
	result = uds_allocate_cache_aligned(bytes, "search list", &list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	list->capacity = capacity;
	list->first_dead_entry = 0;

	for (i = 0; i < capacity; i++) {
		list->entries[i] = i;
	}

	*list_ptr = list;
	return UDS_SUCCESS;
}

static int __must_check initialize_sparse_cache(struct sparse_cache *cache,
						const struct geometry *geometry,
						unsigned int capacity,
						unsigned int zone_count)
{
	unsigned int i;
	int result;

	cache->geometry = geometry;
	cache->capacity = capacity;
	cache->zone_count = zone_count;

	/*
	 * Scale down the skip threshold since the cache only counts cache
	 * misses in zone zero, but requests are being handled in all zones.
	 */
	cache->skip_search_threshold = (SKIP_SEARCH_THRESHOLD / zone_count);

	result = uds_initialize_barrier(&cache->begin_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = uds_initialize_barrier(&cache->end_cache_update, zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	for (i = 0; i < capacity; i++) {
		result = initialize_cached_chapter_index(&cache->chapters[i],
							 geometry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	for (i = 0; i < zone_count; i++) {
		result = make_search_list(capacity, &cache->search_lists[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

int make_sparse_cache(const struct geometry *geometry,
		      unsigned int capacity,
		      unsigned int zone_count,
		      struct sparse_cache **cache_ptr)
{
	unsigned int bytes =
		(sizeof(struct sparse_cache) +
		 (capacity * sizeof(struct cached_chapter_index)));

	struct sparse_cache *cache;
	int result = uds_allocate_cache_aligned(bytes, "sparse cache", &cache);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result =
		initialize_sparse_cache(cache, geometry, capacity, zone_count);
	if (result != UDS_SUCCESS) {
		free_sparse_cache(cache);
		return result;
	}

	*cache_ptr = cache;
	return UDS_SUCCESS;
}

size_t get_sparse_cache_memory_size(const struct sparse_cache *cache)
{
	/*
	 * Count the delta_index_page as cache memory, but ignore all other
	 * overhead.
	 */
	size_t page_size = (sizeof(struct delta_index_page) +
			    cache->geometry->bytes_per_page);
	size_t chapter_size =
		(page_size * cache->geometry->index_pages_per_chapter);
	return (cache->capacity * chapter_size);
}

static INLINE void set_skip_search(struct cached_chapter_index *chapter,
				   bool skip_search)
{
	/* Check before setting to reduce cache line contention. */
	if (READ_ONCE(chapter->skip_search) != skip_search) {
		WRITE_ONCE(chapter->skip_search, skip_search);
	}
}

static void score_chapter_hit(struct sparse_cache *cache,
			      struct cached_chapter_index *chapter)
{
	cache->counters.chapter_hits += 1;
	set_skip_search(chapter, false);
}

static void score_chapter_miss(struct sparse_cache *cache)
{
	cache->counters.chapter_misses += 1;
}

static void score_eviction(struct index_zone *zone,
			   struct sparse_cache *cache,
			   struct cached_chapter_index *chapter)
{
	if (chapter->virtual_chapter == UINT64_MAX) {
		return;
	}
	if (chapter->virtual_chapter < zone->oldest_virtual_chapter) {
		cache->counters.invalidations += 1;
	} else {
		cache->counters.evictions += 1;
	}
}

static void score_search_hit(struct sparse_cache *cache,
			     struct cached_chapter_index *chapter)
{
	cache->counters.search_hits += 1;
	chapter->counters.search_hits += 1;
	chapter->counters.consecutive_misses = 0;
	set_skip_search(chapter, false);
}

static void score_search_miss(struct sparse_cache *cache,
			      struct cached_chapter_index *chapter)
{
	cache->counters.search_misses += 1;
	chapter->counters.search_misses += 1;
	chapter->counters.consecutive_misses += 1;
	if (chapter->counters.consecutive_misses >
	    cache->skip_search_threshold) {
		set_skip_search(chapter, true);
	}
}

static void release_cached_chapter_index(struct cached_chapter_index *chapter)
{
	if (chapter->volume_buffers != NULL) {
		unsigned int i;

		for (i = 0; i < chapter->index_pages_count; i++) {
			if (chapter->volume_buffers[i] != NULL) {
				dm_bufio_release(chapter->volume_buffers[i]);
				chapter->volume_buffers[i] = NULL;
			}
		}
	}
}

static void destroy_cached_chapter_index(struct cached_chapter_index *chapter)
{
	release_cached_chapter_index(chapter);
	UDS_FREE(chapter->index_pages);
	UDS_FREE(chapter->volume_buffers);
}

void free_sparse_cache(struct sparse_cache *cache)
{
	unsigned int i;

	if (cache == NULL) {
		return;
	}

	for (i = 0; i < cache->zone_count; i++) {
		UDS_FREE(UDS_FORGET(cache->search_lists[i]));
	}

	for (i = 0; i < cache->capacity; i++) {
		struct cached_chapter_index *chapter = &cache->chapters[i];

		destroy_cached_chapter_index(chapter);
	}

	uds_destroy_barrier(&cache->begin_cache_update);
	uds_destroy_barrier(&cache->end_cache_update);
	UDS_FREE(cache);
}

#ifdef TEST_INTERNAL
struct cache_counters
get_sparse_cache_counters(const struct sparse_cache *cache)
{
	struct cache_counters counters = {
		.sparse_chapters = {
			.hits      = cache->counters.chapter_hits,
			.misses    = cache->counters.chapter_misses,
		},
		.sparse_searches = {
			.hits      = cache->counters.search_hits,
			.misses    = cache->counters.search_misses,
		},
		.evictions   = cache->counters.evictions,
		.expirations = cache->counters.invalidations,
	};
	return counters;
}
#endif /* TEST_INTERNAL */

static INLINE struct search_list_iterator
iterate_search_list(struct search_list *list,
		    struct cached_chapter_index chapters[])
{
	struct search_list_iterator iterator = {
		.list = list,
		.next_entry = 0,
		.chapters = chapters,
	};
	return iterator;
}

static INLINE bool
has_next_chapter(const struct search_list_iterator *iterator)
{
	return (iterator->next_entry < iterator->list->first_dead_entry);
}

static INLINE struct cached_chapter_index *
get_next_chapter(struct search_list_iterator *iterator)
{
	return &iterator->chapters[iterator->list
					   ->entries[iterator->next_entry++]];
}

/*
 * Take the element of the search list at the end of the prefix and move it to
 * the start, pushing the pointers previously before it back down the list.
 */
static INLINE uint8_t rotate_search_list(struct search_list *search_list,
					 uint8_t prefix_length)
{
	uint8_t most_recent = search_list->entries[prefix_length - 1];

	if (prefix_length > 1) {
		memmove(&search_list->entries[1],
			&search_list->entries[0],
			prefix_length - 1);
		search_list->entries[0] = most_recent;
	}

	/*
	 * This function may have moved a dead chapter to the front of the list
	 * for reuse, in which case the set of dead chapters becomes smaller.
	 */
	if (search_list->first_dead_entry < prefix_length) {
		search_list->first_dead_entry += 1;
	}

	return most_recent;
}

bool sparse_cache_contains(struct sparse_cache *cache,
			   uint64_t virtual_chapter,
			   unsigned int zone_number)
{
	/*
	 * The correctness of the barriers depends on the invariant that
	 * between calls to update_sparse_cache(), the answers this function
	 * returns must never vary: the result for a given chapter must be
	 * identical across zones. That invariant must be maintained even if
	 * the chapter falls off the end of the volume, or if searching it is
	 * disabled because of too many search misses.
	 */
	struct search_list_iterator iterator =
		iterate_search_list(cache->search_lists[zone_number],
				    cache->chapters);
	while (has_next_chapter(&iterator)) {
		struct cached_chapter_index *chapter =
			get_next_chapter(&iterator);
		if (virtual_chapter == chapter->virtual_chapter) {
			if (zone_number == ZONE_ZERO) {
				score_chapter_hit(cache, chapter);
			}

			rotate_search_list(iterator.list, iterator.next_entry);
			return true;
		}
	}

	/* The specified virtual chapter isn't cached. */
	if (zone_number == ZONE_ZERO) {
		score_chapter_miss(cache);
	}
	return false;
}

/*
 * Resort cache entries into three sets (active, skippable, and dead) while
 * maintaining the LRU ordering that already existed. This operation must only
 * be called during the critical section in update_sparse_cache().
 */
static void purge_search_list(struct search_list *search_list,
			      const struct cached_chapter_index chapters[],
			      uint64_t oldest_virtual_chapter)
{
	uint8_t *entries, *alive, *skipped, *dead;
	unsigned int next_alive, next_skipped, next_dead;
	int i;

	if (search_list->first_dead_entry == 0) {
		return;
	}

	entries = &search_list->entries[0];
	alive = &entries[search_list->capacity];
	skipped = &alive[search_list->capacity];
	dead = &skipped[search_list->capacity];
	next_alive = next_skipped = next_dead = 0;

	for (i = 0; i < search_list->first_dead_entry; i++) {
		uint8_t entry = entries[i];
		const struct cached_chapter_index *chapter = &chapters[entry];

		if ((chapter->virtual_chapter < oldest_virtual_chapter) ||
		    (chapter->virtual_chapter == UINT64_MAX)) {
			dead[next_dead++] = entry;
		} else if (chapter->skip_search) {
			skipped[next_skipped++] = entry;
		} else {
			alive[next_alive++] = entry;
		}
	}

	memcpy(entries, alive, next_alive);
	entries += next_alive;

	memcpy(entries, skipped, next_skipped);
	entries += next_skipped;

	memcpy(entries, dead, next_dead);
	search_list->first_dead_entry = (next_alive + next_skipped);
}

static int __must_check
cache_chapter_index(struct cached_chapter_index *chapter,
		    uint64_t virtual_chapter,
		    const struct volume *volume)
{
	int result;
	/* Mark the cached chapter as unused in case the update fails midway. */
	chapter->virtual_chapter = UINT64_MAX;
	release_cached_chapter_index(chapter);

	result = read_chapter_index_from_volume(volume,
						virtual_chapter,
						chapter->volume_buffers,
						chapter->index_pages);
	if (result != UDS_SUCCESS) {
		return result;
	}

	chapter->counters.search_hits = 0;
	chapter->counters.search_misses = 0;
	chapter->counters.consecutive_misses = 0;

	chapter->virtual_chapter = virtual_chapter;
	chapter->skip_search = false;

	return UDS_SUCCESS;
}

static INLINE void copy_search_list(const struct search_list *source,
				    struct search_list *target)
{
	*target = *source;
	memcpy(target->entries, source->entries, source->capacity);
}

/*
 * Update the sparse cache to contain a chapter index. This function must be
 * called by all the zone threads with the same chapter number to correctly
 * enter the thread barriers used to synchronize the cache updates.
 */
int update_sparse_cache(struct index_zone *zone, uint64_t virtual_chapter)
{
	int result = UDS_SUCCESS;
	const struct uds_index *index = zone->index;
	struct sparse_cache *cache = index->volume->sparse_cache;

	if (sparse_cache_contains(cache, virtual_chapter, zone->id)) {
		return UDS_SUCCESS;
	}

	/*
	 * Wait for every zone thread to reach its corresponding barrier
	 * request and invoke this function before starting to modify the
	 * cache.
	 */
	uds_enter_barrier(&cache->begin_cache_update, NULL);

	/*
	 * This is the start of the critical section: the zone zero thread is
	 * captain, effectively holding an exclusive lock on the sparse cache.
	 * All the other zone threads must do nothing between the two barriers.
	 * They will wait at the end_cache_update barrier for the captain to
	 * finish the update.
	 */

	if (zone->id == ZONE_ZERO) {
		unsigned int z;
		struct search_list *zone_zero_list =
			cache->search_lists[ZONE_ZERO];
		purge_search_list(zone_zero_list,
				  cache->chapters,
				  zone->oldest_virtual_chapter);

		if (virtual_chapter >= index->oldest_virtual_chapter) {
			struct cached_chapter_index *victim =
				&cache->chapters[rotate_search_list(zone_zero_list,
								    cache->capacity)];

			score_eviction(zone, cache, victim);
			result = cache_chapter_index(victim, virtual_chapter,
						     index->volume);
		}

		for (z = 1; z < cache->zone_count; z++) {
			copy_search_list(zone_zero_list,
					 cache->search_lists[z]);
		}
	}

	/*
	 * This is the end of the critical section. All cache invariants must
	 * have been restored.
	 */
	uds_enter_barrier(&cache->end_cache_update, NULL);
	return result;
}

void invalidate_sparse_cache(struct sparse_cache *cache)
{
	unsigned int i;

	if (cache == NULL) {
		return;
	}
	for (i = 0; i < cache->capacity; i++) {
		struct cached_chapter_index *chapter = &cache->chapters[i];

		chapter->virtual_chapter = UINT64_MAX;
		release_cached_chapter_index(chapter);
	}
}

static INLINE bool
should_skip_chapter_index(const struct index_zone *zone,
		          const struct cached_chapter_index *chapter,
		          uint64_t virtual_chapter)
{
	if ((chapter->virtual_chapter == UINT64_MAX) ||
	    (chapter->virtual_chapter < zone->oldest_virtual_chapter)) {
		return true;
	}

	if (virtual_chapter != UINT64_MAX) {
		return (virtual_chapter != chapter->virtual_chapter);
	} else {
		return READ_ONCE(chapter->skip_search);
	}
}

static int __must_check
search_cached_chapter_index(struct cached_chapter_index *chapter,
			    const struct geometry *geometry,
			    const struct index_page_map *index_page_map,
			    const struct uds_chunk_name *name,
			    int *record_page_ptr)
{
	unsigned int physical_chapter =
		map_to_physical_chapter(geometry, chapter->virtual_chapter);
	unsigned int index_page_number =
		find_index_page_number(index_page_map, name, physical_chapter);

	return search_chapter_index_page(&chapter->index_pages[index_page_number],
				         geometry,
				         name,
				         record_page_ptr);
}

int search_sparse_cache(struct index_zone *zone,
			const struct uds_chunk_name *name,
			uint64_t *virtual_chapter_ptr,
			int *record_page_ptr)
{
	struct volume *volume = zone->index->volume;
	struct sparse_cache *cache = volume->sparse_cache;
	unsigned int zone_number = zone->id;
	/* Search the entire cache unless a specific chapter was requested. */
	bool search_all = (*virtual_chapter_ptr == UINT64_MAX);

	struct search_list_iterator iterator =
		iterate_search_list(cache->search_lists[zone_number],
				    cache->chapters);
	while (has_next_chapter(&iterator)) {
		int result;
		struct cached_chapter_index *chapter =
			get_next_chapter(&iterator);

		if (should_skip_chapter_index(zone, chapter,
					      *virtual_chapter_ptr)) {
			continue;
		}

		result = search_cached_chapter_index(chapter,
						     cache->geometry,
						     volume->index_page_map,
						     name,
						     record_page_ptr);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (*record_page_ptr != NO_CHAPTER_INDEX_ENTRY) {
			if (zone_number == ZONE_ZERO) {
				score_search_hit(cache, chapter);
			}

			rotate_search_list(iterator.list, iterator.next_entry);

			/*
			 * In theory, this might be a false match while a true
			 * match exists in another chapter, but that's a very
			 * rare case and not worth the extra search complexity.
			 */
			*virtual_chapter_ptr = chapter->virtual_chapter;
			return UDS_SUCCESS;
		}

		if (zone_number == ZONE_ZERO) {
			score_search_miss(cache, chapter);
		}

		if (!search_all) {
			break;
		}
	}

	*record_page_ptr = NO_CHAPTER_INDEX_ENTRY;
	return UDS_SUCCESS;
}
