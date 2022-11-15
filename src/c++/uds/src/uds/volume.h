/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUME_H
#define VOLUME_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/dm-bufio.h>

#include "chapter-index.h"
#include "common.h"
#include "config.h"
#include "geometry.h"
#include "index-layout.h"
#include "index-page-map.h"
#include "permassert.h"
#include "radix-sort.h"
#include "sparse-cache.h"
#include "uds.h"
#include "uds-threads.h"

/*
 * The volume manages deduplication records on permanent storage. The term
 * "volume" can also refer to the region of permanent storage where the records
 * (and the chapters containing them) are stored. The volume handles all I/O to
 * this region by reading, caching, and writing chapter pages as necessary.
 */

enum reader_state {
	READER_STATE_RUN = 1,
	READER_STATE_EXIT = 2,
	READER_STATE_STOP = 4
};

enum index_lookup_mode {
	/* Always do lookups in all chapters normally */
	LOOKUP_NORMAL,
	/* Only do a subset of lookups needed when rebuilding an index */
	LOOKUP_FOR_REBUILD,
};

enum {
	VOLUME_CACHE_MAX_ENTRIES = (UINT16_MAX >> 1),
	VOLUME_CACHE_QUEUED_FLAG = (1 << 15),
	VOLUME_CACHE_MAX_QUEUED_READS = 4096,
};

struct queued_read {
	bool invalid;
	bool reserved;
	unsigned int physical_page;
	struct uds_request *first_request;
	struct uds_request *last_request;
};

/*
 * The invalidate counter is two 32 bits fields stored together atomically. The
 * low order 32 bits are the physical page number of the cached page being
 * read. The high order 32 bits are a sequence number. This value is written
 * when the zone that owns it begins or completes a cache search. Any other
 * thread will only read the counter in wait_for_pending_searches() while
 * waiting to update the cache contents.
 */
typedef int64_t invalidate_counter_t;

/* These masks select the fields of an invalidate_counter_t. */
/* The mask for the page number field */
#define PAGE_FIELD ((long) UINT_MAX)
/* The mask for the LSB of the counter field */
#define COUNTER_LSB (PAGE_FIELD + 1L)

struct __aligned(L1_CACHE_BYTES) search_pending_counter {
	atomic64_t atomic_value;
};

struct cached_page {
	/* Whether this page is currently being read asynchronously */
	bool cp_read_pending;
	/* The physical page stored in this cache entry */
	unsigned int cp_physical_page;
	/* The value of the volume clock when this page was last used */
	int64_t cp_last_used;
	/* The cached page buffer */
	struct dm_buffer *buffer;
	/* The chapter index page, meaningless for record pages */
	struct delta_index_page cp_index_page;
};

struct page_cache {
	/* Geometry governing the volume */
	const struct geometry *geometry;
	/* The number of zones */
	unsigned int zone_count;
	/* The current number of cache entries */
	unsigned int num_index_entries;
	/* The maximum number of cached entries */
	uint16_t num_cache_entries;
	/* An index for each physical page noting where it is in the cache */
	uint16_t *index;
	/* The array of cached pages */
	struct cached_page *cache;
	/* A counter for each zone tracking if a search is occurring there */
	struct search_pending_counter *search_pending_counters;
	/* The read queue entries as a circular array */
	struct queued_read *read_queue;

	/* All entries above this point are constant after initialization. */

	/*
	 * These values are all indexes into the array of read queue entries.
	 * New entries in the read queue are enqueued at read_queue_last. To
	 * dequeue entries, a reader thread gets the lock and then claims the
	 * entry pointed to by read_queue_last_read and increments that value.
	 * After the read is completed, the reader thread calls
	 * release_read_queue_entry(), which increments read_queue_first until
	 * it points to a pending read, or is equal to read_queue_last_read.
	 * This means that if multiple reads are outstanding, read_queue_first
	 * might not advance until the last of the reads finishes.
	 */
	uint16_t read_queue_first;
	uint16_t read_queue_last_read;
	uint16_t read_queue_last;

	atomic64_t clock;
};

struct volume {
	struct geometry *geometry;
	struct dm_bufio_client *client;
	uint64_t nonce;

	/* A single page worth of records, for sorting */
	const struct uds_volume_record **record_pointers;
	/* Sorter for sorting records within each page */
	struct radix_sorter *radix_sorter;

	struct sparse_cache *sparse_cache;
	struct page_cache *page_cache;
	struct index_page_map *index_page_map;

	struct mutex read_threads_mutex;
	struct cond_var read_threads_cond;
	struct cond_var read_threads_read_done_cond;
	struct thread **reader_threads;
	unsigned int num_read_threads;
	enum reader_state reader_state;

	enum index_lookup_mode lookup_mode;
	unsigned int reserved_buffers;
};

#ifdef TEST_INTERNAL
typedef void (*request_restarter_t)(struct uds_request *);

void set_request_restarter(request_restarter_t restarter);

int encode_record_page(const struct volume *volume,
		       const struct uds_volume_record records[],
		       byte record_page[]);

bool search_record_page(const byte record_page[],
			const struct uds_record_name *name,
			const struct geometry *geometry,
			struct uds_record_data *metadata);

#endif /* TEST_INTERNAL*/
int __must_check make_volume(const struct configuration *config,
			     struct index_layout *layout,
			     struct volume **new_volume);

void free_volume(struct volume *volume);

int __must_check replace_volume_storage(struct volume *volume,
					struct index_layout *layout,
					const char *path);

int __must_check make_page_cache(const struct geometry *geometry,
				 unsigned int chapters_in_cache,
				 unsigned int zone_count,
				 struct page_cache **cache_ptr);

void free_page_cache(struct page_cache *cache);

void invalidate_page_cache(struct page_cache *cache);

int __must_check
invalidate_page_cache_for_chapter(struct page_cache *cache,
				  unsigned int chapter,
				  unsigned int pages_per_chapter);

#ifdef TEST_INTERNAL
int find_invalidate_and_make_least_recent(struct page_cache *cache,
					  unsigned int physical_page,
					  bool must_find);

#endif /* TEST_INTERNAL */
void make_page_most_recent(struct page_cache *cache,
			   struct cached_page *page_ptr);

int __must_check assert_page_in_cache(struct page_cache *cache,
				      struct cached_page *page);

int __must_check get_page_from_cache(struct page_cache *cache,
				     unsigned int physical_page,
				     struct cached_page **page);

int __must_check enqueue_read(struct page_cache *cache,
			      struct uds_request *request,
			      unsigned int physical_page);

int __must_check select_victim_in_cache(struct page_cache *cache,
					struct cached_page **page_ptr);

int __must_check put_page_in_cache(struct page_cache *cache,
				   unsigned int physical_page,
				   struct cached_page *page);

void cancel_page_in_cache(struct page_cache *cache,
			  unsigned int physical_page,
			  struct cached_page *page);

size_t __must_check get_page_cache_size(struct page_cache *cache);

static inline invalidate_counter_t
get_invalidate_counter(struct page_cache *cache, unsigned int zone_number)
{
	return atomic64_read(&cache->search_pending_counters[zone_number].atomic_value);
}

static inline void set_invalidate_counter(struct page_cache *cache,
					  unsigned int zone_number,
					  invalidate_counter_t invalidate_counter)
{
	atomic64_set(&cache->search_pending_counters[zone_number].atomic_value,
		     invalidate_counter);
}

static inline unsigned int searched_page(invalidate_counter_t counter)
{
	return counter & PAGE_FIELD;
}

static inline bool search_pending(invalidate_counter_t invalidate_counter)
{
	return (invalidate_counter & COUNTER_LSB) != 0;
}

/* Lock the cache for a zone in order to search for a page. */
static inline void begin_pending_search(struct page_cache *cache,
					unsigned int physical_page,
					unsigned int zone_number)
{
	invalidate_counter_t invalidate_counter =
		get_invalidate_counter(cache, zone_number);
	invalidate_counter &= ~PAGE_FIELD;
	invalidate_counter |= physical_page;
	invalidate_counter += COUNTER_LSB;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
	ASSERT_LOG_ONLY(search_pending(invalidate_counter),
			"Search is pending for zone %u",
			zone_number);
	/*
	 * This memory barrier ensures that the write to the invalidate counter
	 * is seen by other threads before this thread accesses the cached
	 * page. The corresponding read memory barrier is in
	 * wait_for_pending_searches().
	 */
	smp_mb();
}

/* Unlock the cache for a zone by clearing its invalidate counter. */
static inline void end_pending_search(struct page_cache *cache,
				      unsigned int zone_number)
{
	invalidate_counter_t invalidate_counter;
	/*
	 * This memory barrier ensures that this thread completes reads of the
	 * cached page before other threads see the write to the invalidate
	 * counter.
	 */
	smp_mb();

	invalidate_counter = get_invalidate_counter(cache, zone_number);
	ASSERT_LOG_ONLY(search_pending(invalidate_counter),
			"Search is pending for zone %u",
			zone_number);
	invalidate_counter += COUNTER_LSB;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
}

int __must_check enqueue_page_read(struct volume *volume,
				   struct uds_request *request,
				   int physical_page);

int __must_check find_volume_chapter_boundaries(struct volume *volume,
						uint64_t *lowest_vcn,
						uint64_t *highest_vcn,
						bool *is_empty);

int __must_check search_volume_page_cache(struct volume *volume,
					  struct uds_request *request,
					  const struct uds_record_name *name,
					  uint64_t virtual_chapter,
					  struct uds_record_data *metadata,
					  bool *found);

int __must_check search_cached_record_page(struct volume *volume,
					   struct uds_request *request,
					   const struct uds_record_name *name,
					   unsigned int chapter,
					   int record_page_number,
					   struct uds_record_data *duplicate,
					   bool *found);

int __must_check forget_chapter(struct volume *volume, uint64_t chapter);

int __must_check write_index_pages(struct volume *volume,
				   int physical_page,
				   struct open_chapter_index *chapter_index,
				   byte **pages);

int __must_check write_record_pages(struct volume *volume,
				    int physical_page,
				    const struct uds_volume_record *records,
				    byte **pages);

int __must_check write_chapter(struct volume *volume,
			       struct open_chapter_index *chapter_index,
			       const struct uds_volume_record records[]);

int __must_check
read_chapter_index_from_volume(const struct volume *volume,
			       uint64_t virtual_chapter,
			       struct dm_buffer *volume_buffers[],
			       struct delta_index_page index_pages[]);

int __must_check get_volume_page_locked(struct volume *volume,
					unsigned int physical_page,
					struct cached_page **entry_ptr);

int __must_check get_volume_page_protected(struct volume *volume,
					   struct uds_request *request,
					   unsigned int physical_page,
					   struct cached_page **entry_ptr);

int __must_check get_volume_page(struct volume *volume,
				 unsigned int chapter,
				 unsigned int page_number,
				 byte **data_ptr,
				 struct delta_index_page **index_page_ptr);

size_t __must_check get_cache_size(struct volume *volume);

int __must_check
find_volume_chapter_boundaries_impl(unsigned int chapter_limit,
				    unsigned int max_bad_chapters,
				    uint64_t *lowest_vcn,
				    uint64_t *highest_vcn,
				    int (*probe_func)(void *aux,
						      unsigned int chapter,
						      uint64_t *vcn),
				    struct geometry *geometry,
				    void *aux);

int __must_check map_to_physical_page(const struct geometry *geometry,
				      int chapter,
				      int page);

#endif /* VOLUME_H */
