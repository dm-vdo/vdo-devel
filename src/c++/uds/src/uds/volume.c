// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "volume.h"

#include <linux/atomic.h>
#include <linux/dm-bufio.h>
#include <linux/err.h>

#include "chapter-index.h"
#include "config.h"
#ifdef TEST_INTERNAL
#include "dory.h"
#endif /* TEST_INTERNAL */
#include "errors.h"
#include "geometry.h"
#include "hash-utils.h"
#include "index.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "sparse-cache.h"
#include "string-utils.h"
#include "uds-threads.h"

/*
 * The first block of the volume layout is reserved for the volume header, which is no longer used.
 * The remainder of the volume is divided into chapters consisting of several pages of records, and
 * several pages of static index to use to find those records. The index pages are recorded first,
 * followed by the record pages. The chapters are written in order as they are filled, so the
 * volume storage acts as a circular log of the most recent chapters, with each new chapter
 * overwriting the oldest saved one.
 *
 * When a new chapter is filled and closed, the records from that chapter are sorted and
 * interleaved in approximate temporal order, and assigned to record pages. Then a static delta
 * index is generated to store which record page contains each record. The in-memory index page map
 * is also updated to indicate which delta lists fall on each chapter index page. This means that
 * when a record is read, the volume only has to load a single index page and a single record page,
 * rather than search the entire chapter. These index and record pages are written to storage, and
 * the index pages are transferred to the page cache under the theory that the most recently
 * written chapter is likely to be accessed again soon.
 *
 * When reading a record, the volume index will indicate which chapter should contain it. The
 * volume uses the index page map to determine which chapter index page needs to be loaded, and
 * then reads the relevant record page number from the chapter index. Both index and record pages
 * are stored in a page cache when read for the common case that subsequent records need the same
 * pages. The page cache evicts the least recently accessed entries when caching new pages. In
 * addition, the volume uses dm-bufio to manage access to the storage, which may allow for
 * additional caching depending on available system resources.
 *
 * Record requests are handled from cached pages when possible. If a page needs to be read, it is
 * placed on a queue along with the request that wants to read it. Any requests for the same page
 * that arrive while the read is pending are added to the queue entry. A separate reader thread
 * handles the queued reads, adding the page to the cache and updating any requests queued with it
 * so they can continue processing. This allows the index zone threads to continue processing new
 * requests rather than wait for the storage reads.
 *
 * When an index rebuild is necessary, the volume reads each stored chapter to determine which
 * range of chapters contain valid records, so that those records can be used to reconstruct the
 * in-memory volume index.
 */

enum {
	/* The maximum allowable number of contiguous bad chapters */
	MAX_BAD_CHAPTERS = 100,
};

#ifdef TEST_INTERNAL
/* This function pointer allows unit tests to intercept the slow-lane requeuing of a request. */
static request_restarter_t request_restarter;

void set_request_restarter(request_restarter_t restarter)
{
	request_restarter = restarter;
}

#endif /* TEST_INTERNAL */
static inline unsigned int
map_to_page_number(struct geometry *geometry, unsigned int physical_page)
{
	return (physical_page - HEADER_PAGES_PER_VOLUME) % geometry->pages_per_chapter;
}

static inline unsigned int
map_to_chapter_number(struct geometry *geometry, unsigned int physical_page)
{
	return (physical_page - HEADER_PAGES_PER_VOLUME) / geometry->pages_per_chapter;
}

static inline bool is_record_page(struct geometry *geometry, unsigned int physical_page)
{
	return map_to_page_number(geometry, physical_page) >= geometry->index_pages_per_chapter;
}

static inline unsigned int get_zone_number(struct uds_request *request)
{
	return (request == NULL) ? 0 : request->zone_number;
}

int map_to_physical_page(const struct geometry *geometry, int chapter, int page)
{
	/* Page zero is the header page, so the first chapter index page is page one. */
	return HEADER_PAGES_PER_VOLUME + (geometry->pages_per_chapter * chapter) + page;
}

static void release_page_buffer(struct cached_page *page)
{
	if (page->buffer != NULL)
		dm_bufio_release(UDS_FORGET(page->buffer));
}

static void clear_cache_page(struct page_cache *cache, struct cached_page *page)
{
	/* Do not clear read_pending because the read queue relies on it. */
	release_page_buffer(page);
	page->physical_page = cache->indexable_pages;
	WRITE_ONCE(page->last_used, 0);
}

static void get_page_and_index(struct page_cache *cache,
			       unsigned int physical_page,
			       int *queue_index,
			       struct cached_page **page_ptr)
{
	u16 index_value;
	u16 index;
	bool queued;

	/*
	 * ASSERTION: We are either a zone thread holding a search_pending_counter, or we are any
	 * thread holding the read_threads_mutex.
	 *
	 * Holding only a search_pending_counter is the most frequent case.
	 */
	/*
	 * It would be unlikely for the compiler to turn the usage of index_value into two reads of
	 * cache->index, but it would be possible and very bad if those reads did not return the
	 * same bits.
	 */
	index_value = READ_ONCE(cache->index[physical_page]);
	queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;
	index = index_value & ~VOLUME_CACHE_QUEUED_FLAG;

	if (!queued && (index < cache->cache_slots)) {
		*page_ptr = &cache->cache[index];
		/*
		 * We have acquired access to the cached page, but unless we hold the
		 * read_threads_mutex, we need a read memory barrier now. The corresponding write
		 * memory barrier is in put_page_in_cache().
		 */
		smp_rmb();
	} else {
		*page_ptr = NULL;
	}

	*queue_index = queued ? index : -1;
}

static void wait_for_pending_searches(struct page_cache *cache, unsigned int physical_page)
{
	invalidate_counter_t initial_counters[MAX_ZONES];
	unsigned int i;

	/*
	 * We hold the read_threads_mutex. We are waiting for threads that do not hold the
	 * read_threads_mutex. Those threads have "locked" their targeted page by setting the
	 * search_pending_counter. The corresponding write memory barrier is in
	 * begin_pending_search().
	 */
	smp_mb();

	for (i = 0; i < cache->zone_count; i++)
		initial_counters[i] = get_invalidate_counter(cache, i);
	for (i = 0; i < cache->zone_count; i++)
		if (search_pending(initial_counters[i]) &&
		    (searched_page(initial_counters[i]) == physical_page))
			/*
			 * There is an active search using the physical page. We need to wait for
			 * the search to finish.
			 *
			 * FIXME: Investigate using wait_event() to wait for the search to finish.
			 */
			while (initial_counters[i] == get_invalidate_counter(cache, i))
				cond_resched();
}

EXTERNAL_STATIC void invalidate_page(struct page_cache *cache, unsigned int physical_page)
{
	struct cached_page *page;
	int queue_index = -1;

	/* We hold the read_threads_mutex. */
	get_page_and_index(cache, physical_page, &queue_index, &page);
	if (page != NULL) {
		WRITE_ONCE(cache->index[page->physical_page], cache->cache_slots);
		wait_for_pending_searches(cache, page->physical_page);
		clear_cache_page(cache, page);
	} else if (queue_index > -1) {
		uds_log_debug("setting pending read to invalid");
		cache->read_queue[queue_index].invalid = true;
	}
}

static int __must_check initialize_page_cache(struct page_cache *cache,
					      const struct geometry *geometry,
					      unsigned int chapters_in_cache,
					      unsigned int zone_count)
{
	int result;
	unsigned int i;

	cache->geometry = geometry;
	cache->indexable_pages = geometry->pages_per_volume + 1;
	cache->cache_slots = chapters_in_cache * geometry->record_pages_per_chapter;
	cache->zone_count = zone_count;
	atomic64_set(&cache->clock, 1);

	result = UDS_ALLOCATE(VOLUME_CACHE_MAX_QUEUED_READS,
			      struct queued_read,
			      "volume read queue",
			      &cache->read_queue);
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE(cache->zone_count,
			      struct search_pending_counter,
			      "Volume Cache Zones",
			      &cache->search_pending_counters);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT((cache->cache_slots <= VOLUME_CACHE_MAX_ENTRIES),
			"requested cache size, %u, within limit %u",
			cache->cache_slots,
			VOLUME_CACHE_MAX_ENTRIES);
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE(cache->indexable_pages, u16, "page cache index", &cache->index);
	if (result != UDS_SUCCESS)
		return result;

	/* Initialize index values to invalid values. */
	for (i = 0; i < cache->indexable_pages; i++)
		cache->index[i] = cache->cache_slots;

	result = UDS_ALLOCATE(cache->cache_slots,
			      struct cached_page,
			      "page cache cache",
			      &cache->cache);
	if (result != UDS_SUCCESS)
		return result;

	for (i = 0; i < cache->cache_slots; i++)
		clear_cache_page(cache, &cache->cache[i]);

	return UDS_SUCCESS;
}

int make_page_cache(const struct geometry  *geometry,
		    unsigned int chapters_in_cache,
		    unsigned int zone_count,
		    struct page_cache **cache_ptr)
{
	struct page_cache *cache;
	int result;

	result = UDS_ALLOCATE(1, struct page_cache, "volume cache", &cache);
	if (result != UDS_SUCCESS)
		return result;

	result = initialize_page_cache(cache, geometry, chapters_in_cache, zone_count);
	if (result != UDS_SUCCESS) {
		free_page_cache(cache);
		return result;
	}

	*cache_ptr = cache;
	return UDS_SUCCESS;
}

void free_page_cache(struct page_cache *cache)
{
	if (cache == NULL)
		return;

	if (cache->cache != NULL) {
		unsigned int i;

		for (i = 0; i < cache->cache_slots; i++)
			release_page_buffer(&cache->cache[i]);
	}
	UDS_FREE(cache->index);
	UDS_FREE(cache->cache);
	UDS_FREE(cache->search_pending_counters);
	UDS_FREE(cache->read_queue);
	UDS_FREE(cache);
}

void invalidate_page_cache(struct page_cache *cache)
{
	unsigned int i;

	for (i = 0; i < cache->indexable_pages; i++)
		cache->index[i] = cache->cache_slots;

	for (i = 0; i < cache->cache_slots; i++)
		clear_cache_page(cache, &cache->cache[i]);
}

void invalidate_page_cache_for_chapter(struct page_cache *cache, unsigned int chapter)
{
	unsigned int i;
	unsigned int first_page = map_to_physical_page(cache->geometry, chapter, 0);

	/* We hold the read_threads_mutex. */
	for (i = 0; i < cache->geometry->pages_per_chapter; i++)
		invalidate_page(cache, first_page + i);
}

EXTERNAL_STATIC void make_page_most_recent(struct page_cache *cache, struct cached_page *page)
{
	/*
	 * ASSERTION: We are either a zone thread holding a search_pending_counter, or we are any
	 * thread holding the read_threads_mutex.
	 */
	if (atomic64_read(&cache->clock) != READ_ONCE(page->last_used))
		WRITE_ONCE(page->last_used,
			   atomic64_inc_return(&cache->clock));
}

static int __must_check
get_least_recent_page(struct page_cache *cache, struct cached_page **page_ptr)
{
	/* We hold the read_threads_mutex. */
	int oldest_index = 0;
	unsigned int i;

	for (i = 0;; i++) {
		if (i >= cache->cache_slots)
			/* This should never happen. */
			return ASSERT(false, "oldest page is not NULL");

		/* A page with a pending read must not be replaced. */
		if (!cache->cache[i].read_pending) {
			oldest_index = i;
			break;
		}
	}

	for (i = 0; i < cache->cache_slots; i++) {
		if (!cache->cache[i].read_pending &&
		    (READ_ONCE(cache->cache[i].last_used) <=
		     READ_ONCE(cache->cache[oldest_index].last_used)))
			oldest_index = i;
	}

	*page_ptr = &cache->cache[oldest_index];
	return UDS_SUCCESS;
}

EXTERNAL_STATIC void get_page_from_cache(struct page_cache *cache,
					 unsigned int physical_page,
					 struct cached_page **page)
{
	/*
	 * ASSERTION: We are in a zone thread.
	 * ASSERTION: We holding a search_pending_counter or the read_threads_mutex.
	 */
	int queue_index = -1;

	get_page_and_index(cache, physical_page, &queue_index, page);
}

/* Select a page to remove from the cache to make space for a new entry. */
EXTERNAL_STATIC int
select_victim_in_cache(struct page_cache *cache, struct cached_page **page_ptr)
{
	struct cached_page *page = NULL;
	int result;

	/* We hold the read_threads_mutex. */
	result = get_least_recent_page(cache, &page);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT((page != NULL), "least recent page was not NULL");
	if (result != UDS_SUCCESS)
		return result;

	if (page->physical_page != cache->indexable_pages) {
		WRITE_ONCE(cache->index[page->physical_page], cache->cache_slots);
		wait_for_pending_searches(cache, page->physical_page);
	}

	page->read_pending = true;
	clear_cache_page(cache, page);
	*page_ptr = page;
	return UDS_SUCCESS;
}

/* Make a newly filled cache entry available to other threads. */
EXTERNAL_STATIC int
put_page_in_cache(struct page_cache *cache, unsigned int physical_page, struct cached_page *page)
{
	u16 value;
	int result;

	/* We hold the read_threads_mutex. */
	result = ASSERT((page->read_pending), "page to install has a pending read");
	if (result != UDS_SUCCESS)
		return result;

	page->physical_page = physical_page;

	value = page - cache->cache;
	result = ASSERT((value < cache->cache_slots), "cache index is valid");
	if (result != UDS_SUCCESS)
		return result;

	make_page_most_recent(cache, page);
	page->read_pending = false;

	/*
	 * We hold the read_threads_mutex, but we must have a write memory barrier before making
	 * the cached_page available to the readers that do not hold the mutex. The corresponding
	 * read memory barrier is in get_page_and_index().
	 */
	smp_wmb();

	/* This assignment also clears the queued flag. */
	WRITE_ONCE(cache->index[physical_page], value);
	return UDS_SUCCESS;
}

static void cancel_page_in_cache(struct page_cache *cache,
				 unsigned int physical_page,
				 struct cached_page *page)
{
	int result;

	/* We hold the read_threads_mutex. */
	result = ASSERT((page->read_pending), "page to install has a pending read");
	if (result != UDS_SUCCESS)
		return;

	clear_cache_page(cache, page);
	page->read_pending = false;

	/* Clear the mapping and the queued flag for the new page. */
	WRITE_ONCE(cache->index[physical_page], cache->cache_slots);
}

static inline u16 next_queue_position(u16 position)
{
	return (position + 1) % VOLUME_CACHE_MAX_QUEUED_READS;
}

static inline void advance_queue_position(u16 *position)
{
	*position = next_queue_position(*position);
}

static inline bool read_queue_is_full(struct page_cache *cache)
{
	return cache->read_queue_first == next_queue_position(cache->read_queue_last);
}

EXTERNAL_STATIC int
enqueue_read(struct page_cache *cache, struct uds_request *request, unsigned int physical_page)
{
	int result;
	struct queued_read *queue_entry;
	u16 last = cache->read_queue_last;
	u16 read_queue_index;

	/* We hold the read_threads_mutex. */
	if ((cache->index[physical_page] & VOLUME_CACHE_QUEUED_FLAG) == 0) {
		/* This page has no existing entry in the queue. */
		if (read_queue_is_full(cache))
			return UDS_SUCCESS;

		/* Fill in the read queue entry. */
		cache->read_queue[last].physical_page = physical_page;
		cache->read_queue[last].invalid = false;
		cache->read_queue[last].first_request = NULL;
		cache->read_queue[last].last_request = NULL;

		/* Point the cache index to the read queue entry. */
		read_queue_index = last;
		WRITE_ONCE(cache->index[physical_page],
			   read_queue_index | VOLUME_CACHE_QUEUED_FLAG);

		advance_queue_position(&cache->read_queue_last);
	} else {
		/* It's already queued, so add to the existing entry. */
		read_queue_index = cache->index[physical_page] & ~VOLUME_CACHE_QUEUED_FLAG;
	}

	result = ASSERT((read_queue_index < VOLUME_CACHE_MAX_QUEUED_READS),
			"queue is not overfull");
	if (result != UDS_SUCCESS)
		return result;

	request->next_request = NULL;
	queue_entry = &cache->read_queue[read_queue_index];
	if (queue_entry->first_request == NULL)
		queue_entry->first_request = request;
	else
		queue_entry->last_request->next_request = request;
	queue_entry->last_request = request;

	return UDS_QUEUED;
}

static void wait_for_read_queue_not_full(struct volume *volume, struct uds_request *request)
{
	unsigned int zone_number = get_zone_number(request);
	invalidate_counter_t invalidate_counter =
		get_invalidate_counter(volume->page_cache, zone_number);

	if (search_pending(invalidate_counter))
		/*
		 * Release any search_pending lock to avoid deadlock where the reader threads
		 * cannot make progress because they are waiting on the counter and the index
		 * thread cannot because the read queue is full.
		 */
		end_pending_search(volume->page_cache, zone_number);

	while (read_queue_is_full(volume->page_cache)) {
		uds_log_debug("Waiting until read queue not full");
		uds_signal_cond(&volume->read_threads_cond);
		uds_wait_cond(&volume->read_threads_read_done_cond, &volume->read_threads_mutex);
	}

	if (search_pending(invalidate_counter))
		/* Reacquire the search_pending lock released earlier. */
		begin_pending_search(volume->page_cache,
				     searched_page(invalidate_counter),
				     zone_number);
}

int enqueue_page_read(struct volume *volume, struct uds_request *request, int physical_page)
{
	int result;

	/* Don't allow new requests if we are shutting down. */
	if ((volume->reader_state & READER_STATE_EXIT) != 0) {
		uds_log_info("failed to queue read while shutting down");
		return -EBUSY;
	}

	/*
	 * Mark the page as queued in the volume cache, for chapter invalidation to be able to
	 * cancel a read. If we are unable to do this because the queues are full, flush them
	 * first.
	 */
	while ((result = enqueue_read(volume->page_cache,
				      request,
				      physical_page)) == UDS_SUCCESS) {
		uds_log_debug("Read queues full, waiting for reads to finish");
		wait_for_read_queue_not_full(volume, request);
	}

	if (result == UDS_QUEUED)
		uds_signal_cond(&volume->read_threads_cond);

	return result;
}

/*
 * Reserve the next read queue entry for processing, but do not actually remove it from the queue.
 * Must be followed by release_queued_requests().
 */
static struct queued_read *reserve_read_queue_entry(struct page_cache *cache)
{
	/* We hold the read_threads_mutex. */
	struct queued_read *entry;
	u16 index_value;
	bool queued;

	/* No items to dequeue */
	if (cache->read_queue_last_read == cache->read_queue_last)
		return NULL;

	entry = &cache->read_queue[cache->read_queue_last_read];
	index_value = cache->index[entry->physical_page];
	queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;
	/* Check to see if it's still queued before resetting. */
	if (entry->invalid && queued)
		WRITE_ONCE(cache->index[entry->physical_page], cache->cache_slots);

	/*
	 * If a synchronous read has taken this page, set invalid to true so it doesn't get
	 * overwritten. Requests will just be requeued.
	 */
	if (!queued)
		entry->invalid = true;

	entry->reserved = true;
	advance_queue_position(&cache->read_queue_last_read);
	return entry;
}

static inline struct queued_read *wait_to_reserve_read_queue_entry(struct volume *volume)
{
	struct queued_read *queue_entry = NULL;

	while ((volume->reader_state & READER_STATE_EXIT) == 0) {
#ifdef TEST_INTERNAL
		if ((volume->reader_state & READER_STATE_STOP) != 0) {
			uds_wait_cond(&volume->read_threads_cond, &volume->read_threads_mutex);
			continue;
		}

#endif /* TEST_INTERNAL */
		queue_entry = reserve_read_queue_entry(volume->page_cache);
		if (queue_entry != NULL)
			break;

		uds_wait_cond(&volume->read_threads_cond, &volume->read_threads_mutex);
	}

	return queue_entry;
}

static int init_chapter_index_page(const struct volume *volume,
				   u8 *index_page,
				   unsigned int chapter,
				   unsigned int index_page_number,
				   struct delta_index_page *chapter_index_page)
{
	u64 ci_virtual;
	unsigned int ci_chapter;
	unsigned int lowest_list;
	unsigned int highest_list;
	struct geometry *geometry = volume->geometry;
	int result;

	result = initialize_chapter_index_page(chapter_index_page,
					       geometry,
					       index_page,
					       volume->nonce);
	if (volume->lookup_mode == LOOKUP_FOR_REBUILD)
		return result;

	if (result != UDS_SUCCESS)
		return uds_log_error_strerror(result,
					      "Reading chapter index page for chapter %u page %u",
					      chapter,
					      index_page_number);

	get_list_number_bounds(volume->index_page_map,
			       chapter,
			       index_page_number,
			       &lowest_list,
			       &highest_list);
	ci_virtual = chapter_index_page->virtual_chapter_number;
	ci_chapter = map_to_physical_chapter(geometry, ci_virtual);
	if ((chapter == ci_chapter) &&
	    (lowest_list == chapter_index_page->lowest_list_number) &&
	    (highest_list == chapter_index_page->highest_list_number))
		return UDS_SUCCESS;

	uds_log_warning("Index page map updated to %llu",
			(unsigned long long) volume->index_page_map->last_update);
	uds_log_warning("Page map expects that chapter %u page %u has range %u to %u, but chapter index page has chapter %llu with range %u to %u",
			chapter,
			index_page_number,
			lowest_list,
			highest_list,
			(unsigned long long) ci_virtual,
			chapter_index_page->lowest_list_number,
			chapter_index_page->highest_list_number);
	ASSERT_LOG_ONLY(false, "index page map mismatch with chapter index");
	return UDS_CORRUPT_DATA;
}

static int initialize_index_page(const struct volume *volume,
				 unsigned int physical_page,
				 struct cached_page *page)
{
	unsigned int chapter = map_to_chapter_number(volume->geometry, physical_page);
	unsigned int index_page_number = map_to_page_number(volume->geometry, physical_page);

	return init_chapter_index_page(volume,
				       dm_bufio_get_block_data(page->buffer),
				       chapter,
				       index_page_number,
				       &page->index_page);
}

EXTERNAL_STATIC bool search_record_page(const u8 record_page[],
					const struct uds_record_name *name,
					const struct geometry *geometry,
					struct uds_record_data *metadata)
{
	/*
	 * The array of records is sorted by name and stored as a binary tree in heap order, so the
	 * root of the tree is the first array element.
	 */
	unsigned int node = 0;
	const struct uds_volume_record *records = (const struct uds_volume_record *) record_page;

	while (node < geometry->records_per_page) {
		int result;
		const struct uds_volume_record *record = &records[node];

		result = memcmp(name, &record->name, UDS_RECORD_NAME_SIZE);
		if (result == 0) {
			if (metadata != NULL)
				*metadata = record->data;
			return true;
		}

		/* The children of node N are at indexes 2N+1 and 2N+2. */
		node = ((2 * node) + ((result < 0) ? 1 : 2));
	}

	return false;
}

/*
 * If we've read in a record page, we're going to do an immediate search, to speed up processing by
 * avoiding get_record_from_zone(), and to ensure that requests make progress even when queued. If
 * we've read in an index page, we save the record page number so we don't have to resolve the
 * index page again. We use the location, virtual_chapter, and old_metadata fields in the request
 * to allow the index code to know where to begin processing the request again.
 */
static int search_page(struct cached_page *page,
		       const struct volume *volume,
		       struct uds_request *request,
		       unsigned int physical_page)
{
	int result;
	enum uds_index_region location;
	int record_page_number;

	if (is_record_page(volume->geometry, physical_page)) {
		if (search_record_page(dm_bufio_get_block_data(page->buffer),
				       &request->record_name,
				       volume->geometry,
				       &request->old_metadata))
			location = UDS_LOCATION_RECORD_PAGE_LOOKUP;
		else
			location = UDS_LOCATION_UNAVAILABLE;
	} else {
		result = search_chapter_index_page(&page->index_page,
						   volume->geometry,
						   &request->record_name,
						   &record_page_number);
		if (result != UDS_SUCCESS)
			return result;

		if (record_page_number == NO_CHAPTER_INDEX_ENTRY) {
			location = UDS_LOCATION_UNAVAILABLE;
		} else {
			location = UDS_LOCATION_INDEX_PAGE_LOOKUP;
			*((int *) &request->old_metadata) = record_page_number;
		}
	}

	request->location = location;
	request->found = false;
	return UDS_SUCCESS;
}

static int process_entry(struct volume *volume, struct queued_read *entry)
{
	unsigned int page_number = entry->physical_page;
	struct uds_request *request;
	struct cached_page *page = NULL;
	u8 *page_data;
	int result;

	if (entry->invalid) {
		uds_log_debug("Requeuing requests for invalid page");
		return UDS_SUCCESS;
	}

	result = select_victim_in_cache(volume->page_cache, &page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error selecting cache victim for page read");
		return result;
	}

	uds_unlock_mutex(&volume->read_threads_mutex);
	page_data = dm_bufio_read(volume->client, page_number, &page->buffer);
	if (IS_ERR(page_data)) {
		result = -PTR_ERR(page_data);
		uds_log_warning_strerror(result,
					 "error reading physical page %u from volume",
					 page_number);
		cancel_page_in_cache(volume->page_cache, page_number, page);
		return result;
	}
	uds_lock_mutex(&volume->read_threads_mutex);

	if (entry->invalid) {
		uds_log_warning("Page %u invalidated after read", page_number);
		cancel_page_in_cache(volume->page_cache, page_number, page);
		return UDS_SUCCESS;
	}

	if (!is_record_page(volume->geometry, page_number)) {
		result = initialize_index_page(volume, page_number, page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error initializing chapter index page");
			cancel_page_in_cache(volume->page_cache, page_number, page);
			return result;
		}
	}

	result = put_page_in_cache(volume->page_cache, page_number, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", page_number);
		cancel_page_in_cache(volume->page_cache, page_number, page);
		return result;
	}

	request = entry->first_request;
	while ((request != NULL) && (result == UDS_SUCCESS)) {
		result = search_page(page, volume, request, page_number);
		request = request->next_request;
	}

	return result;
}

static void release_queued_requests(struct volume *volume, struct queued_read *entry, int result)
{
	struct page_cache *cache = volume->page_cache;
	u16 last_read = cache->read_queue_last_read;
	struct uds_request *request;
	struct uds_request *next;

	for (request = entry->first_request; request != NULL; request = next) {
		next = request->next_request;
		request->status = result;
		request->requeued = true;
#ifdef TEST_INTERNAL
		if (request_restarter != NULL) {
			request_restarter(request);
			continue;
		}
#endif /* TEST_INTERNAL*/
		enqueue_request(request, STAGE_INDEX);
	}

	entry->reserved = false;

	/* Move the read_queue_first pointer as far as we can. */
	while ((cache->read_queue_first != last_read) &&
	       (!cache->read_queue[cache->read_queue_first].reserved))
		advance_queue_position(&cache->read_queue_first);
	uds_broadcast_cond(&volume->read_threads_read_done_cond);
}

static void read_thread_function(void *arg)
{
	struct volume *volume = arg;

	uds_log_debug("reader starting");
	uds_lock_mutex(&volume->read_threads_mutex);
	while (true) {
		struct queued_read *queue_entry;
		int result;

		queue_entry = wait_to_reserve_read_queue_entry(volume);
		if ((volume->reader_state & READER_STATE_EXIT) != 0)
			break;

		result = process_entry(volume, queue_entry);
		release_queued_requests(volume, queue_entry, result);
	}
	uds_unlock_mutex(&volume->read_threads_mutex);
	uds_log_debug("reader done");
}

static int read_page_locked(struct volume *volume,
			    struct uds_request *request,
			    unsigned int physical_page,
			    struct cached_page **page_ptr)
{
	int result = UDS_SUCCESS;
	struct cached_page *page = NULL;
	u8 *page_data;

	if ((request != NULL) && (request->session != NULL))
		return enqueue_page_read(volume, request, physical_page);

	result = select_victim_in_cache(volume->page_cache, &page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error selecting cache victim for page read");
		return result;
	}

	page_data = dm_bufio_read(volume->client, physical_page, &page->buffer);
	if (IS_ERR(page_data)) {
		result = -PTR_ERR(page_data);
		uds_log_warning_strerror(result,
					 "error reading physical page %u from volume",
					 physical_page);
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	if (!is_record_page(volume->geometry, physical_page)) {
		result = initialize_index_page(volume, physical_page, page);
		if (result != UDS_SUCCESS) {
			if (volume->lookup_mode != LOOKUP_FOR_REBUILD)
				uds_log_warning("Corrupt index page %u", physical_page);
			cancel_page_in_cache(volume->page_cache, physical_page, page);
			return result;
		}
	}

	result = put_page_in_cache(volume->page_cache, physical_page, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache",
				physical_page);
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	*page_ptr = page;

	return UDS_SUCCESS;
}

/* Retrieve a page from the cache while holding the read threads mutex. */
EXTERNAL_STATIC int get_volume_page_locked(struct volume *volume,
					   unsigned int physical_page,
					   struct cached_page **page_ptr)
{
	int result;
	struct cached_page *page = NULL;

	get_page_from_cache(volume->page_cache, physical_page, &page);
	if (page == NULL) {
		result = read_page_locked(volume, NULL, physical_page, &page);
		if (result != UDS_SUCCESS)
			return result;
	} else {
		make_page_most_recent(volume->page_cache, page);
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

/* Retrieve a page from the cache while holding a search_pending lock. */
EXTERNAL_STATIC int get_volume_page_protected(struct volume *volume,
					      struct uds_request *request,
					      unsigned int physical_page,
					      struct cached_page **page_ptr)
{
	int result;
	unsigned int zone_number;
	struct cached_page *page = NULL;

	get_page_from_cache(volume->page_cache, physical_page, &page);

	zone_number = get_zone_number(request);
	if (page != NULL) {
		if (zone_number == 0)
			/* Only one zone is allowed to update the LRU. */
			make_page_most_recent(volume->page_cache, page);

		*page_ptr = page;
		return UDS_SUCCESS;
	}

	/* Prepare to enqueue a read for the page. */
	end_pending_search(volume->page_cache, zone_number);
	uds_lock_mutex(&volume->read_threads_mutex);

	/*
	 * Do the lookup again while holding the read mutex (no longer the fast case so this should
	 * be fine to repeat). We need to do this because a page may have been added to the cache
	 * by a reader thread between the time we searched above and the time we went to actually
	 * try to enqueue it below. This could result in us enqueuing another read for a page which
	 * is already in the cache, which would mean we end up with two entries in the cache for
	 * the same page.
	 */
	get_page_from_cache(volume->page_cache, physical_page, &page);
	if (page == NULL) {
		result = read_page_locked(volume, request, physical_page, &page);
		if (result != UDS_SUCCESS) {
			/*
			 * This code path is used frequently in the UDS_QUEUED case, so the
			 * performance gain from unlocking first, while "search pending" mode is
			 * off, turns out to be significant in some cases.
			 */
			uds_unlock_mutex(&volume->read_threads_mutex);
			begin_pending_search(volume->page_cache, physical_page, zone_number);
			return result;
		}
	}

	/*
	 * Now that the page is loaded, the volume needs to switch to "reader thread unlocked" and
	 * "search pending" state in careful order so no other thread can mess with the data before
	 * the caller gets to look at it.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);
	uds_unlock_mutex(&volume->read_threads_mutex);
	*page_ptr = page;
	return UDS_SUCCESS;
}

static int get_volume_page(struct volume *volume,
			   unsigned int chapter,
			   unsigned int page_number,
			   struct cached_page **page_ptr)
{
	int result;
	unsigned int physical_page = map_to_physical_page(volume->geometry, chapter, page_number);

	uds_lock_mutex(&volume->read_threads_mutex);
	result = get_volume_page_locked(volume, physical_page, page_ptr);
	uds_unlock_mutex(&volume->read_threads_mutex);
	return result;
}

int get_volume_record_page(struct volume *volume,
			   unsigned int chapter,
			   unsigned int page_number,
			   u8 **data_ptr)
{
	int result;
	struct cached_page *page = NULL;

	result = get_volume_page(volume, chapter, page_number, &page);
	if (result == UDS_SUCCESS)
		*data_ptr = dm_bufio_get_block_data(page->buffer);
	return result;
}

int get_volume_index_page(struct volume *volume,
			  unsigned int chapter,
			  unsigned int page_number,
			  struct delta_index_page **index_page_ptr)
{
	int result;
	struct cached_page *page = NULL;

	result = get_volume_page(volume, chapter, page_number, &page);
	if (result == UDS_SUCCESS)
		*index_page_ptr = &page->index_page;
	return result;
}

/*
 * Find the record page associated with a name in a given index page. This will return UDS_QUEUED
 * if the page in question must be read from storage.
 */
static int search_cached_index_page(struct volume *volume,
				    struct uds_request *request,
				    const struct uds_record_name *name,
				    unsigned int chapter,
				    unsigned int index_page_number,
				    int *record_page_number)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int zone_number = get_zone_number(request);
	unsigned int physical_page =
		map_to_physical_page(volume->geometry, chapter, index_page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read the mapping. This
	 * prevents this thread from reading a page in the cache which has already been marked for
	 * invalidation by the reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume, request, physical_page, &page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	result = ASSERT(search_pending(get_invalidate_counter(volume->page_cache, zone_number)),
			"Search is pending for zone %u",
			zone_number);
	if (result != UDS_SUCCESS)
		return result;

	result = search_chapter_index_page(&page->index_page,
					   volume->geometry,
					   name,
					   record_page_number);
	end_pending_search(volume->page_cache, zone_number);
	return result;
}

/*
 * Find the metadata associated with a name in a given record page. This will return UDS_QUEUED if
 * the page in question must be read from storage.
 */
int search_cached_record_page(struct volume *volume,
			      struct uds_request *request,
			      const struct uds_record_name *name,
			      unsigned int chapter,
			      int record_page_number,
			      struct uds_record_data *duplicate,
			      bool *found)
{
	struct cached_page *record_page;
	struct geometry *geometry = volume->geometry;
	int physical_page, result;
	unsigned int page_number, zone_number;

	*found = false;
	if (record_page_number == NO_CHAPTER_INDEX_ENTRY)
		return UDS_SUCCESS;

	result = ASSERT(((record_page_number >= 0) &&
			 ((unsigned int) record_page_number < geometry->record_pages_per_chapter)),
			"0 <= %d <= %u",
			record_page_number,
			geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS)
		return result;

	page_number = geometry->index_pages_per_chapter + record_page_number;

	zone_number = get_zone_number(request);
	physical_page = map_to_physical_page(volume->geometry, chapter, page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read the mapping. This
	 * prevents this thread from reading a page in the cache which has already been marked for
	 * invalidation by the reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume, request, physical_page, &record_page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	if (search_record_page(dm_bufio_get_block_data(record_page->buffer),
			       name,
			       geometry,
			       duplicate))
		*found = true;

	end_pending_search(volume->page_cache, zone_number);
	return UDS_SUCCESS;
}

int read_chapter_index_from_volume(const struct volume *volume,
				   u64 virtual_chapter,
				   struct dm_buffer *volume_buffers[],
				   struct delta_index_page index_pages[])
{
	int result;
	unsigned int i;
	const struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter = map_to_physical_chapter(geometry, virtual_chapter);
	int physical_page = map_to_physical_page(geometry, physical_chapter, 0);

	dm_bufio_prefetch(volume->client, physical_page, geometry->index_pages_per_chapter);
	for (i = 0; i < geometry->index_pages_per_chapter; i++) {
		u8 *index_page;

		index_page = dm_bufio_read(volume->client, physical_page + i, &volume_buffers[i]);
		if (IS_ERR(index_page)) {
			result = -PTR_ERR(index_page);
			uds_log_warning_strerror(result,
						 "error reading physical page %u",
						 physical_page);
			return result;
		}

		result = init_chapter_index_page(volume,
						 index_page,
						 physical_chapter,
						 i,
						 &index_pages[i]);
		if (result != UDS_SUCCESS)
			return result;
	}

	return UDS_SUCCESS;
}

int search_volume_page_cache(struct volume *volume,
			     struct uds_request *request,
			     const struct uds_record_name *name,
			     u64 virtual_chapter,
			     struct uds_record_data *metadata,
			     bool *found)
{
	int result;
	unsigned int physical_chapter = map_to_physical_chapter(volume->geometry, virtual_chapter);
	unsigned int index_page_number;
	int record_page_number;

	index_page_number = find_index_page_number(volume->index_page_map, name, physical_chapter);

	if ((request != NULL) && (request->location == UDS_LOCATION_INDEX_PAGE_LOOKUP)) {
		record_page_number = *((int *) &request->old_metadata);
	} else {
		result = search_cached_index_page(volume,
						  request,
						  name,
						  physical_chapter,
						  index_page_number,
						  &record_page_number);
		if (result != UDS_SUCCESS)
			return result;
	}

	return search_cached_record_page(volume,
					 request,
					 name,
					 physical_chapter,
					 record_page_number,
					 metadata,
					 found);
}

void forget_chapter(struct volume *volume, u64 virtual_chapter)
{
	unsigned int physical_chapter = map_to_physical_chapter(volume->geometry, virtual_chapter);

	uds_log_debug("forgetting chapter %llu", (unsigned long long) virtual_chapter);
	uds_lock_mutex(&volume->read_threads_mutex);
	invalidate_page_cache_for_chapter(volume->page_cache, physical_chapter);
	uds_unlock_mutex(&volume->read_threads_mutex);
}

/*
 * Donate an index pages from a newly written chapter to the page cache since it is likely to be
 * used again soon. The caller must already hold the reader thread mutex.
 */
static int donate_index_page_locked(struct volume *volume,
				    unsigned int physical_chapter,
				    unsigned int index_page_number,
				    struct dm_buffer *page_buffer)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int physical_page =
		map_to_physical_page(volume->geometry, physical_chapter, index_page_number);

	result = select_victim_in_cache(volume->page_cache, &page);
	if (result != UDS_SUCCESS) {
		dm_bufio_release(page_buffer);
		return result;
	}

	page->buffer = page_buffer;
	result = init_chapter_index_page(volume,
					 dm_bufio_get_block_data(page_buffer),
					 physical_chapter,
					 index_page_number,
					 &page->index_page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error initialize chapter index page");
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	result = put_page_in_cache(volume->page_cache, physical_page, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", physical_page);
		cancel_page_in_cache(volume->page_cache, physical_page, page);
		return result;
	}

	return UDS_SUCCESS;
}

int write_index_pages(struct volume *volume,
		      int physical_page,
		      struct open_chapter_index *chapter_index,
		      u8 **pages)
{
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry, chapter_index->virtual_chapter_number);
	unsigned int delta_list_number = 0;
	unsigned int index_page_number;

	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		u8 *page_data;
		u32 lists_packed;
		bool last_page;
		int result;

		page_data = dm_bufio_new(volume->client,
					 physical_page + index_page_number,
					 &page_buffer);
		if (IS_ERR(page_data))
			return uds_log_warning_strerror(-PTR_ERR(page_data),
							"failed to prepare index page");

		last_page = ((index_page_number + 1) == geometry->index_pages_per_chapter);
		result = pack_open_chapter_index_page(chapter_index,
						      page_data,
						      delta_list_number,
						      last_page,
						      &lists_packed);
		if (result != UDS_SUCCESS) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(result, "failed to pack index page");
		}

#ifdef TEST_INTERNAL
		if (get_dory_forgetful()) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(-EROFS,
							"failed to write chapter index page");
		}

#endif /* TEST_INTERNAL */
		dm_bufio_mark_buffer_dirty(page_buffer);

		if (pages != NULL)
			memcpy(pages[index_page_number], page_data, geometry->bytes_per_page);

		if (lists_packed == 0)
			uds_log_debug("no delta lists packed on chapter %u page %u",
				      physical_chapter_number,
				      index_page_number);
		else
			delta_list_number += lists_packed;

		update_index_page_map(volume->index_page_map,
				      chapter_index->virtual_chapter_number,
				      physical_chapter_number,
				      index_page_number,
				      delta_list_number - 1);

		uds_lock_mutex(&volume->read_threads_mutex);
		result = donate_index_page_locked(volume,
						  physical_chapter_number,
						  index_page_number,
						  page_buffer);
		uds_unlock_mutex(&volume->read_threads_mutex);
		if (result != UDS_SUCCESS) {
			dm_bufio_release(page_buffer);
			return result;
		}
	}

	return UDS_SUCCESS;
}

static unsigned int encode_tree(u8 record_page[],
				const struct uds_volume_record *sorted_pointers[],
				unsigned int next_record,
				unsigned int node,
				unsigned int node_count)
{
	if (node < node_count) {
		unsigned int child = (2 * node) + 1;

		next_record = encode_tree(record_page,
					  sorted_pointers,
					  next_record,
					  child,
					  node_count);

		/*
		 * In-order traversal: copy the contents of the next record into the page at the
		 * node offset.
		 */
		memcpy(&record_page[node * BYTES_PER_RECORD],
		       sorted_pointers[next_record++],
		       BYTES_PER_RECORD);

		next_record = encode_tree(record_page,
					  sorted_pointers,
					  next_record,
					  child + 1,
					  node_count);
	}

	return next_record;
}

EXTERNAL_STATIC int encode_record_page(const struct volume *volume,
				       const struct uds_volume_record records[],
				       u8 record_page[])
{
	int result;
	unsigned int i;
	unsigned int records_per_page = volume->geometry->records_per_page;
	const struct uds_volume_record **record_pointers = volume->record_pointers;

	for (i = 0; i < records_per_page; i++)
		record_pointers[i] = &records[i];

	/*
	 * Sort the record pointers by using just the names in the records, which is less work than
	 * sorting the entire record values.
	 */
	STATIC_ASSERT(offsetof(struct uds_volume_record, name) == 0);
	result = uds_radix_sort(volume->radix_sorter,
				(const u8 **) record_pointers,
				records_per_page,
				UDS_RECORD_NAME_SIZE);
	if (result != UDS_SUCCESS)
		return result;

	encode_tree(record_page, record_pointers, 0, 0, records_per_page);
	return UDS_SUCCESS;
}

int write_record_pages(struct volume *volume,
		       int physical_page,
		       const struct uds_volume_record *records,
		       u8 **pages)
{
	unsigned int record_page_number;
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	const struct uds_volume_record *next_record = records;

	/* Skip over the index pages, which have already been written. */
	physical_page += geometry->index_pages_per_chapter;

	for (record_page_number = 0;
	     record_page_number < geometry->record_pages_per_chapter;
	     record_page_number++) {
		u8 *page_data;
		int result;

		page_data = dm_bufio_new(volume->client,
					 physical_page + record_page_number,
					 &page_buffer);
		if (IS_ERR(page_data))
			return uds_log_warning_strerror(-PTR_ERR(page_data),
							"failed to prepare record page");

		result = encode_record_page(volume, next_record, page_data);
		if (result != UDS_SUCCESS) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(result,
							"failed to encode record page %u",
							record_page_number);
		}

		next_record += geometry->records_per_page;
#ifdef TEST_INTERNAL
		if (get_dory_forgetful()) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(-EROFS,
							"failed to write chapter record page");
		}

#endif /* TEST_INTERNAL */
		dm_bufio_mark_buffer_dirty(page_buffer);
		if (pages != NULL)
			memcpy(pages[record_page_number], page_data, geometry->bytes_per_page);

		dm_bufio_release(page_buffer);
	}

	return UDS_SUCCESS;
}

int write_chapter(struct volume *volume,
		  struct open_chapter_index *chapter_index,
		  const struct uds_volume_record *records)
{
	struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry, chapter_index->virtual_chapter_number);
	int physical_page = map_to_physical_page(geometry, physical_chapter_number, 0);
	int result;

	result = write_index_pages(volume, physical_page, chapter_index, NULL);
	if (result != UDS_SUCCESS)
		return result;

	result = write_record_pages(volume, physical_page, records, NULL);
	if (result != UDS_SUCCESS)
		return result;

	result = -dm_bufio_write_dirty_buffers(volume->client);
	if (result != UDS_SUCCESS)
		uds_log_error_strerror(result, "cannot sync chapter to volume");

	return result;
}

static int probe_chapter(struct volume *volume,
			 unsigned int chapter_number,
			 u64 *virtual_chapter_number)
{
	const struct geometry *geometry = volume->geometry;
	unsigned int expected_list_number = 0;
	unsigned int i;
	u64 vcn, last_vcn = U64_MAX;

	dm_bufio_prefetch(volume->client,
			  map_to_physical_page(geometry, chapter_number, 0),
			  geometry->index_pages_per_chapter);

	for (i = 0; i < geometry->index_pages_per_chapter; ++i) {
		struct delta_index_page *page;
		int result;

		result = get_volume_index_page(volume, chapter_number, i, &page);
		if (result != UDS_SUCCESS)
			return result;

		vcn = page->virtual_chapter_number;
		if (last_vcn == U64_MAX) {
			last_vcn = vcn;
		} else if (vcn != last_vcn) {
			uds_log_error("inconsistent chapter %u index page %u: expected vcn %llu, got vcn %llu",
				      chapter_number,
				      i,
				      (unsigned long long) last_vcn,
				      (unsigned long long) vcn);
			return UDS_CORRUPT_DATA;
		}

		if (expected_list_number != page->lowest_list_number) {
			uds_log_error("inconsistent chapter %u index page %u: expected list number %u, got list number %u",
				      chapter_number,
				      i,
				      expected_list_number,
				      page->lowest_list_number);
			return UDS_CORRUPT_DATA;
		}
		expected_list_number = page->highest_list_number + 1;

		result = validate_chapter_index_page(page, geometry);
		if (result != UDS_SUCCESS)
			return result;
	}

	if (last_vcn == U64_MAX) {
		uds_log_error("no chapter %u virtual chapter number determined", chapter_number);
		return UDS_CORRUPT_DATA;
	}

	if (chapter_number != map_to_physical_chapter(geometry, last_vcn)) {
		uds_log_error("chapter %u vcn %llu is out of phase (%u)",
			      chapter_number,
			      (unsigned long long) last_vcn,
			      geometry->chapters_per_volume);
		return UDS_CORRUPT_DATA;
	}

	*virtual_chapter_number = last_vcn;
	return UDS_SUCCESS;
}

static int probe_wrapper(void *aux, unsigned int chapter_number, u64 *virtual_chapter_number)
{
	struct volume *volume = aux;
	int result;

	result = probe_chapter(volume, chapter_number, virtual_chapter_number);
	if (result == UDS_CORRUPT_DATA) {
		*virtual_chapter_number = U64_MAX;
		return UDS_SUCCESS;
	}

	return result;
}

/* Find the last valid physical chapter in the volume. */
static int
find_real_end_of_volume(struct volume *volume, unsigned int limit, unsigned int *limit_ptr)
{
	unsigned int span = 1;
	unsigned int tries = 0;

	while (limit > 0) {
		int result;
		unsigned int chapter = (span > limit) ? 0 : limit - span;
		u64 vcn = 0;

		result = probe_chapter(volume, chapter, &vcn);
		if (result == UDS_SUCCESS) {
			if (span == 1)
				break;
			span /= 2;
			tries = 0;
		} else if (result == UDS_CORRUPT_DATA) {
			limit = chapter;
			if (++tries > 1)
				span *= 2;
		} else {
			return uds_log_error_strerror(result, "cannot determine end of volume");
		}
	}

	*limit_ptr = limit;
	return UDS_SUCCESS;
}

/*
 * Find the highest and lowest contiguous chapters present in the volume and determine their
 * virtual chapter numbers. This is used by rebuild.
 */
int find_volume_chapter_boundaries(struct volume *volume,
				   u64 *lowest_vcn,
				   u64 *highest_vcn,
				   bool *is_empty)
{
	unsigned int chapter_limit = volume->geometry->chapters_per_volume;
	int result;

	result = find_real_end_of_volume(volume, chapter_limit, &chapter_limit);
	if (result != UDS_SUCCESS)
		return uds_log_error_strerror(result, "cannot find end of volume");

	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		*is_empty = true;
		return UDS_SUCCESS;
	}

	*is_empty = false;
	return find_volume_chapter_boundaries_impl(chapter_limit,
						   MAX_BAD_CHAPTERS,
						   lowest_vcn,
						   highest_vcn,
						   probe_wrapper,
						   volume->geometry,
						   volume);
}

int find_volume_chapter_boundaries_impl(unsigned int chapter_limit,
					unsigned int max_bad_chapters,
					u64 *lowest_vcn,
					u64 *highest_vcn,
					int (*probe_func)(void *aux,
							  unsigned int chapter,
							  u64 *vcn),
					struct geometry *geometry,
					void *aux)
{
	u64 zero_vcn;
	u64 lowest = U64_MAX;
	u64 highest = U64_MAX;
	u64 moved_chapter = U64_MAX;
	unsigned int left_chapter = 0;
	unsigned int right_chapter = 0;
	unsigned int bad_chapters = 0;
	int result;

	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		return UDS_SUCCESS;
	}

	/*
	 * This method assumes there is at most one run of contiguous bad chapters caused by
	 * unflushed writes. Either the bad spot is at the beginning and end, or somewhere in the
	 * middle. Wherever it is, the highest and lowest VCNs are adjacent to it. Otherwise the
	 * volume is cleanly saved and somewhere in the middle of it the highest VCN immediately
	 * preceeds the lowest one.
	 */

	/* It doesn't matter if this results in a bad spot (U64_MAX). */
	result = (*probe_func)(aux, 0, &zero_vcn);
	if (result != UDS_SUCCESS)
		return result;

	/*
	 * Binary search for end of the discontinuity in the monotonically increasing virtual
	 * chapter numbers; bad spots are treated as a span of U64_MAX values. In effect we're
	 * searching for the index of the smallest value less than zero_vcn. In the case we go off
	 * the end it means that chapter 0 has the lowest vcn.
	 *
	 * If a virtual chapter is out-of-order, it will be the one moved by conversion. Always
	 * skip over the moved chapter when searching, adding it to the range at the end if
	 * necessary.
	 */
	if (geometry->remapped_physical > 0) {
		u64 remapped_vcn;

		result = (*probe_func)(aux, geometry->remapped_physical, &remapped_vcn);
		if (result != UDS_SUCCESS)
			return UDS_SUCCESS;

		if (remapped_vcn == geometry->remapped_virtual)
			moved_chapter = geometry->remapped_physical;
	}

	left_chapter = 0;
	right_chapter = chapter_limit;

	while (left_chapter < right_chapter) {
		u64 probe_vcn;
		unsigned int chapter = (left_chapter + right_chapter) / 2;

		if (chapter == moved_chapter)
			chapter--;

		result = (*probe_func)(aux, chapter, &probe_vcn);
		if (result != UDS_SUCCESS)
			return result;

		if (zero_vcn <= probe_vcn) {
			left_chapter = chapter + 1;
			if (left_chapter == moved_chapter)
				left_chapter++;
		} else {
			right_chapter = chapter;
		}
	}

	result = ASSERT(left_chapter == right_chapter, "left_chapter == right_chapter");
	if (result != UDS_SUCCESS)
		return result;

	left_chapter %= chapter_limit; /* in case we're at the end */

	/* At this point, left_chapter is the chapter with the lowest virtual chapter number. */

	result = (*probe_func)(aux, left_chapter, &lowest);
	if (result != UDS_SUCCESS)
		return result;

	/* The moved chapter might be the lowest in the range. */
	if ((moved_chapter != U64_MAX) && (lowest == geometry->remapped_virtual + 1))
		lowest = geometry->remapped_virtual;

	result = ASSERT((lowest != U64_MAX), "invalid lowest chapter");
	if (result != UDS_SUCCESS)
		return result;

	/*
	 * Circularly scan backwards, moving over any bad chapters until encountering a good one,
	 * which is the chapter with the highest vcn.
	 */

	while (highest == U64_MAX) {
		right_chapter = (right_chapter + chapter_limit - 1) % chapter_limit;
		if (right_chapter == moved_chapter)
			continue;

		result = (*probe_func)(aux, right_chapter, &highest);
		if (result != UDS_SUCCESS)
			return result;

		if (bad_chapters++ >= max_bad_chapters) {
			uds_log_error("too many bad chapters in volume: %u", bad_chapters);
			return UDS_CORRUPT_DATA;
		}
	}

	*lowest_vcn = lowest;
	*highest_vcn = highest;
	return UDS_SUCCESS;
}

static int __must_check allocate_volume(const struct configuration *config,
					struct index_layout *layout,
					struct volume **new_volume)
{
	struct volume *volume;
	struct geometry *geometry;
	unsigned int reserved_buffers;
	int result;

	result = UDS_ALLOCATE(1, struct volume, "volume", &volume);
	if (result != UDS_SUCCESS)
		return result;

	volume->nonce = get_uds_volume_nonce(layout);

	result = copy_geometry(config->geometry, &volume->geometry);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return uds_log_warning_strerror(result, "failed to allocate geometry: error");
	}
	geometry = volume->geometry;

	/*
	 * Reserve a buffer for each entry in the page cache, one for the chapter writer, and one
	 * for each entry in the sparse cache.
	 */
	reserved_buffers = config->cache_chapters * geometry->record_pages_per_chapter;
	reserved_buffers += 1;
	if (is_sparse_geometry(geometry))
		reserved_buffers += (config->cache_chapters * geometry->index_pages_per_chapter);
	volume->reserved_buffers = reserved_buffers;
	result = open_uds_volume_bufio(layout,
				       geometry->bytes_per_page,
				       volume->reserved_buffers,
				       &volume->client);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = make_uds_radix_sorter(geometry->records_per_page, &volume->radix_sorter);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = UDS_ALLOCATE(geometry->records_per_page,
			      const struct uds_volume_record *,
			      "record pointers",
			      &volume->record_pointers);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	if (is_sparse_geometry(geometry)) {
		size_t page_size = sizeof(struct delta_index_page) + geometry->bytes_per_page;

		result = make_sparse_cache(geometry,
					   config->cache_chapters,
					   config->zone_count,
					   &volume->sparse_cache);
		if (result != UDS_SUCCESS) {
			free_volume(volume);
			return result;
		}

		volume->cache_size =
			page_size * geometry->index_pages_per_chapter * config->cache_chapters;
	}
	result = make_page_cache(geometry,
				 config->cache_chapters,
				 config->zone_count,
				 &volume->page_cache);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = make_index_page_map(geometry, &volume->index_page_map);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	volume->cache_size +=
		volume->page_cache->cache_slots * sizeof(struct delta_index_page);
	*new_volume = volume;
	return UDS_SUCCESS;
}

int __must_check
replace_volume_storage(struct volume *volume, struct index_layout *layout, const char *name)
{
	int result;

	result = replace_index_layout_storage(layout, name);
	if (result != UDS_SUCCESS)
		return result;

	/* Release all outstanding dm_bufio objects */
	invalidate_page_cache(volume->page_cache);
	if (volume->sparse_cache != NULL)
		invalidate_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL)
		dm_bufio_client_destroy(UDS_FORGET(volume->client));

	return open_uds_volume_bufio(layout,
				     volume->geometry->bytes_per_page,
				     volume->reserved_buffers,
				     &volume->client);
}

int make_volume(const struct configuration *config,
		struct index_layout *layout,
		struct volume **new_volume)
{
	unsigned int i;
	struct volume *volume = NULL;
	int result;

	result = allocate_volume(config, layout, &volume);
	if (result != UDS_SUCCESS)
		return result;

	result = uds_init_mutex(&volume->read_threads_mutex);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = uds_init_cond(&volume->read_threads_read_done_cond);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = uds_init_cond(&volume->read_threads_cond);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = UDS_ALLOCATE(config->read_threads,
			      struct thread *,
			      "reader threads",
			      &volume->reader_threads);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	for (i = 0; i < config->read_threads; i++) {
		result = uds_create_thread(read_thread_function,
					   (void *) volume,
					   "reader",
					   &volume->reader_threads[i]);
		if (result != UDS_SUCCESS) {
			free_volume(volume);
			return result;
		}

		volume->read_thread_count = i + 1;
	}

	*new_volume = volume;
	return UDS_SUCCESS;
}

void free_volume(struct volume *volume)
{
	if (volume == NULL)
		return;

	if (volume->reader_threads != NULL) {
		unsigned int i;

		/* This works even if some threads weren't started. */
		uds_lock_mutex(&volume->read_threads_mutex);
		volume->reader_state |= READER_STATE_EXIT;
		uds_broadcast_cond(&volume->read_threads_cond);
		uds_unlock_mutex(&volume->read_threads_mutex);
		for (i = 0; i < volume->read_thread_count; i++)
			uds_join_threads(volume->reader_threads[i]);
		UDS_FREE(volume->reader_threads);
		volume->reader_threads = NULL;
	}

	/* Must destroy the client AFTER freeing the caches. */
	free_page_cache(volume->page_cache);
	free_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL)
		dm_bufio_client_destroy(UDS_FORGET(volume->client));

	uds_destroy_cond(&volume->read_threads_cond);
	uds_destroy_cond(&volume->read_threads_read_done_cond);
	uds_destroy_mutex(&volume->read_threads_mutex);
	free_index_page_map(volume->index_page_map);
	free_uds_radix_sorter(volume->radix_sorter);
	UDS_FREE(volume->geometry);
	UDS_FREE(volume->record_pointers);
	UDS_FREE(volume);
}
