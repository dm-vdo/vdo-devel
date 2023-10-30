// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
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
	VOLUME_CACHE_MAX_ENTRIES = (U16_MAX >> 1),
	VOLUME_CACHE_QUEUED_FLAG = (1 << 15),
	VOLUME_CACHE_MAX_QUEUED_READS = 4096,
};

static const u64 BAD_CHAPTER = U64_MAX;

/*
 * The invalidate counter is two 32 bits fields stored together atomically. The low order 32 bits
 * are the physical page number of the cached page being read. The high order 32 bits are a
 * sequence number. This value is written when the zone that owns it begins or completes a cache
 * search. Any other thread will only read the counter in wait_for_pending_searches() while waiting
 * to update the cache contents.
 */
union invalidate_counter {
	u64 value;
	struct {
		u32 page;
		u32 counter;
	};
};

#ifdef TEST_INTERNAL
/* This array, if set, will capture page data when it is encoded by closing chapters. */
u8 **test_pages = NULL;
u32 test_page_count = 0;

/* This function pointer allows unit tests to intercept the slow-lane requeuing of a request. */
static request_restarter_t request_restarter = NULL;

void set_request_restarter(request_restarter_t restarter)
{
	request_restarter = restarter;
}

/* This function pointer allows unit tests to fake reading and testing a chapter for rebuild. */
static chapter_tester_t chapter_tester = NULL;

void set_chapter_tester(chapter_tester_t tester)
{
	chapter_tester = tester;
}

#endif /* TEST_INTERNAL */
static inline u32 map_to_page_number(struct geometry *geometry,
				     u32 physical_page)
{
	return (physical_page - HEADER_PAGES_PER_VOLUME) % geometry->pages_per_chapter;
}

static inline u32 map_to_chapter_number(struct geometry *geometry,
					u32 physical_page)
{
	return (physical_page - HEADER_PAGES_PER_VOLUME) / geometry->pages_per_chapter;
}

static inline bool is_record_page(struct geometry *geometry, u32 physical_page)
{
	return map_to_page_number(geometry, physical_page) >= geometry->index_pages_per_chapter;
}

STATIC u32 map_to_physical_page(const struct geometry *geometry, u32 chapter,
				u32 page)
{
	/* Page zero is the header page, so the first chapter index page is page one. */
	return HEADER_PAGES_PER_VOLUME + (geometry->pages_per_chapter * chapter) + page;
}

static inline union invalidate_counter get_invalidate_counter(struct page_cache *cache,
							      unsigned int zone_number)
{
	return (union invalidate_counter) {
		.value = READ_ONCE(cache->search_pending_counters[zone_number].atomic_value),
	};
}

static inline void set_invalidate_counter(struct page_cache *cache,
					  unsigned int zone_number,
					  union invalidate_counter invalidate_counter)
{
	WRITE_ONCE(cache->search_pending_counters[zone_number].atomic_value,
		   invalidate_counter.value);
}

static inline bool search_pending(union invalidate_counter invalidate_counter)
{
	return (invalidate_counter.counter & 1) != 0;
}

/* Lock the cache for a zone in order to search for a page. */
STATIC void begin_pending_search(struct page_cache *cache, u32 physical_page,
				 unsigned int zone_number)
{
	union invalidate_counter invalidate_counter = get_invalidate_counter(cache, zone_number);

	invalidate_counter.page = physical_page;
	invalidate_counter.counter++;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
	ASSERT_LOG_ONLY(search_pending(invalidate_counter),
			"Search is pending for zone %u",
			zone_number);
	/*
	 * This memory barrier ensures that the write to the invalidate counter is seen by other
	 * threads before this thread accesses the cached page. The corresponding read memory
	 * barrier is in wait_for_pending_searches().
	 */
	smp_mb();
}

/* Unlock the cache for a zone by clearing its invalidate counter. */
STATIC void end_pending_search(struct page_cache *cache,
			       unsigned int zone_number)
{
	union invalidate_counter invalidate_counter;

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
	invalidate_counter.counter++;
	set_invalidate_counter(cache, zone_number, invalidate_counter);
}

static void wait_for_pending_searches(struct page_cache *cache,
				      u32 physical_page)
{
	union invalidate_counter initial_counters[MAX_ZONES];
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
		    (initial_counters[i].page == physical_page))
			/*
			 * There is an active search using the physical page. We need to wait for
			 * the search to finish.
			 *
			 * FIXME: Investigate using wait_event() to wait for the search to finish.
			 */
			while (initial_counters[i].value == get_invalidate_counter(cache, i).value)
				cond_resched();
}

static void release_page_buffer(struct cached_page *page)
{
	if (page->buffer != NULL)
		dm_bufio_release(UDS_FORGET(page->buffer));
}

static void clear_cache_page(struct page_cache *cache,
			     struct cached_page *page)
{
	/* Do not clear read_pending because the read queue relies on it. */
	release_page_buffer(page);
	page->physical_page = cache->indexable_pages;
	WRITE_ONCE(page->last_used, 0);
}

STATIC void make_page_most_recent(struct page_cache *cache,
				  struct cached_page *page)
{
	/*
	 * ASSERTION: We are either a zone thread holding a search_pending_counter, or we are any
	 * thread holding the read_threads_mutex.
	 */
	if (atomic64_read(&cache->clock) != READ_ONCE(page->last_used))
		WRITE_ONCE(page->last_used, atomic64_inc_return(&cache->clock));
}

/* Select a page to remove from the cache to make space for a new entry. */
STATIC struct cached_page *select_victim_in_cache(struct page_cache *cache)
{
	struct cached_page *page;
	int oldest_index = 0;
	s64 oldest_time = S64_MAX;
	s64 last_used;
	u16 i;

	/* Find the oldest unclaimed page. We hold the read_threads_mutex. */
	for (i = 0; i < cache->cache_slots; i++) {
		/* A page with a pending read must not be replaced. */
		if (cache->cache[i].read_pending)
			continue;

		last_used = READ_ONCE(cache->cache[i].last_used);
		if (last_used <= oldest_time) {
			oldest_time = last_used;
			oldest_index = i;
		}
	}

	page = &cache->cache[oldest_index];
	if (page->physical_page != cache->indexable_pages) {
		WRITE_ONCE(cache->index[page->physical_page], cache->cache_slots);
		wait_for_pending_searches(cache, page->physical_page);
	}

	page->read_pending = true;
	clear_cache_page(cache, page);
	return page;
}

/* Make a newly filled cache entry available to other threads. */
STATIC int put_page_in_cache(struct page_cache *cache, u32 physical_page,
			     struct cached_page *page)
{
	int result;

	/* We hold the read_threads_mutex. */
	result = ASSERT((page->read_pending), "page to install has a pending read");
	if (result != UDS_SUCCESS)
		return result;

	page->physical_page = physical_page;
	make_page_most_recent(cache, page);
	page->read_pending = false;

	/*
	 * We hold the read_threads_mutex, but we must have a write memory barrier before making
	 * the cached_page available to the readers that do not hold the mutex. The corresponding
	 * read memory barrier is in get_page_and_index().
	 */
	smp_wmb();

	/* This assignment also clears the queued flag. */
	WRITE_ONCE(cache->index[physical_page], page - cache->cache);
	return UDS_SUCCESS;
}

static void cancel_page_in_cache(struct page_cache *cache, u32 physical_page,
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

STATIC bool enqueue_read(struct page_cache *cache, struct uds_request *request,
			 u32 physical_page)
{
	struct queued_read *queue_entry;
	u16 last = cache->read_queue_last;
	u16 read_queue_index;

	/* We hold the read_threads_mutex. */
	if ((cache->index[physical_page] & VOLUME_CACHE_QUEUED_FLAG) == 0) {
		/* This page has no existing entry in the queue. */
		if (read_queue_is_full(cache))
			return false;

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
		/* It's already queued, so add this request to the existing entry. */
		read_queue_index = cache->index[physical_page] & ~VOLUME_CACHE_QUEUED_FLAG;
	}

	request->next_request = NULL;
	queue_entry = &cache->read_queue[read_queue_index];
	if (queue_entry->first_request == NULL)
		queue_entry->first_request = request;
	else
		queue_entry->last_request->next_request = request;
	queue_entry->last_request = request;

	return true;
}

STATIC void enqueue_page_read(struct volume *volume,
			      struct uds_request *request, u32 physical_page)
{
	/* Mark the page as queued, so that chapter invalidation knows to cancel a read. */
	while (!enqueue_read(&volume->page_cache, request, physical_page)) {
		uds_log_debug("Read queue full, waiting for reads to finish");
#ifdef TEST_INTERNAL
		/* Restart the read threads, which normally only sleep when the queue is empty */
		uds_signal_cond(&volume->read_threads_cond);
#endif /* TEST_INTERNAL */
		uds_wait_cond(&volume->read_threads_read_done_cond, &volume->read_threads_mutex);
	}

	uds_signal_cond(&volume->read_threads_cond);
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
	if (cache->read_queue_next_read == cache->read_queue_last)
		return NULL;

	entry = &cache->read_queue[cache->read_queue_next_read];
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
	advance_queue_position(&cache->read_queue_next_read);
	return entry;
}

static inline struct queued_read *wait_to_reserve_read_queue_entry(struct volume *volume)
{
	struct queued_read *queue_entry = NULL;

	while (!volume->read_threads_exiting) {
#ifdef TEST_INTERNAL
		if (volume->read_threads_stopped) {
			uds_wait_cond(&volume->read_threads_cond, &volume->read_threads_mutex);
			continue;
		}

#endif /* TEST_INTERNAL */
		queue_entry = reserve_read_queue_entry(&volume->page_cache);
		if (queue_entry != NULL)
			break;

		uds_wait_cond(&volume->read_threads_cond, &volume->read_threads_mutex);
	}

	return queue_entry;
}

static int init_chapter_index_page(const struct volume *volume, u8 *index_page,
				   u32 chapter, u32 index_page_number,
				   struct delta_index_page *chapter_index_page)
{
	u64 ci_virtual;
	u32 ci_chapter;
	u32 lowest_list;
	u32 highest_list;
	struct geometry *geometry = volume->geometry;
	int result;

	result = uds_initialize_chapter_index_page(chapter_index_page,
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

	uds_get_list_number_bounds(volume->index_page_map,
				   chapter,
				   index_page_number,
				   &lowest_list,
				   &highest_list);
	ci_virtual = chapter_index_page->virtual_chapter_number;
	ci_chapter = uds_map_to_physical_chapter(geometry, ci_virtual);
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
	return uds_log_error_strerror(UDS_CORRUPT_DATA,
				      "index page map mismatch with chapter index");
}

static int initialize_index_page(const struct volume *volume,
				 u32 physical_page, struct cached_page *page)
{
	u32 chapter = map_to_chapter_number(volume->geometry, physical_page);
	u32 index_page_number = map_to_page_number(volume->geometry, physical_page);

	return init_chapter_index_page(volume,
				       dm_bufio_get_block_data(page->buffer),
				       chapter,
				       index_page_number,
				       &page->index_page);
}

STATIC bool search_record_page(const u8 record_page[],
			       const struct uds_record_name *name,
			       const struct geometry *geometry,
			       struct uds_record_data *metadata)
{
	/*
	 * The array of records is sorted by name and stored as a binary tree in heap order, so the
	 * root of the tree is the first array element.
	 */
	u32 node = 0;
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
static int search_page(struct cached_page *page, const struct volume *volume,
		       struct uds_request *request, u32 physical_page)
{
	int result;
	enum uds_index_region location;
	u16 record_page_number;

	if (is_record_page(volume->geometry, physical_page)) {
		if (search_record_page(dm_bufio_get_block_data(page->buffer),
				       &request->record_name,
				       volume->geometry,
				       &request->old_metadata))
			location = UDS_LOCATION_RECORD_PAGE_LOOKUP;
		else
			location = UDS_LOCATION_UNAVAILABLE;
	} else {
		result = uds_search_chapter_index_page(&page->index_page,
						       volume->geometry,
						       &request->record_name,
						       &record_page_number);
		if (result != UDS_SUCCESS)
			return result;

		if (record_page_number == NO_CHAPTER_INDEX_ENTRY) {
			location = UDS_LOCATION_UNAVAILABLE;
		} else {
			location = UDS_LOCATION_INDEX_PAGE_LOOKUP;
			*((u16 *) &request->old_metadata) = record_page_number;
		}
	}

	request->location = location;
	request->found = false;
	return UDS_SUCCESS;
}

static int process_entry(struct volume *volume, struct queued_read *entry)
{
	u32 page_number = entry->physical_page;
	struct uds_request *request;
	struct cached_page *page = NULL;
	u8 *page_data;
	int result;

	if (entry->invalid) {
		uds_log_debug("Requeuing requests for invalid page");
		return UDS_SUCCESS;
	}

	page = select_victim_in_cache(&volume->page_cache);

	uds_unlock_mutex(&volume->read_threads_mutex);
	page_data = dm_bufio_read(volume->client, page_number, &page->buffer);
	if (IS_ERR(page_data)) {
		result = -PTR_ERR(page_data);
		uds_log_warning_strerror(result,
					 "error reading physical page %u from volume",
					 page_number);
		cancel_page_in_cache(&volume->page_cache, page_number, page);
		return result;
	}
	uds_lock_mutex(&volume->read_threads_mutex);

	if (entry->invalid) {
		uds_log_warning("Page %u invalidated after read", page_number);
		cancel_page_in_cache(&volume->page_cache, page_number, page);
		return UDS_SUCCESS;
	}

	if (!is_record_page(volume->geometry, page_number)) {
		result = initialize_index_page(volume, page_number, page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error initializing chapter index page");
			cancel_page_in_cache(&volume->page_cache, page_number, page);
			return result;
		}
	}

	result = put_page_in_cache(&volume->page_cache, page_number, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", page_number);
		cancel_page_in_cache(&volume->page_cache, page_number, page);
		return result;
	}

	request = entry->first_request;
	while ((request != NULL) && (result == UDS_SUCCESS)) {
		result = search_page(page, volume, request, page_number);
		request = request->next_request;
	}

	return result;
}

static void release_queued_requests(struct volume *volume,
				    struct queued_read *entry, int result)
{
	struct page_cache *cache = &volume->page_cache;
	u16 next_read = cache->read_queue_next_read;
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
		uds_enqueue_request(request, STAGE_INDEX);
	}

	entry->reserved = false;

	/* Move the read_queue_first pointer as far as we can. */
	while ((cache->read_queue_first != next_read) &&
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
		if (volume->read_threads_exiting)
			break;

		result = process_entry(volume, queue_entry);
		release_queued_requests(volume, queue_entry, result);
	}
	uds_unlock_mutex(&volume->read_threads_mutex);
	uds_log_debug("reader done");
}

static void get_page_and_index(struct page_cache *cache, u32 physical_page,
			       int *queue_index, struct cached_page **page_ptr)
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

STATIC void get_page_from_cache(struct page_cache *cache, u32 physical_page,
				struct cached_page **page)
{
	/*
	 * ASSERTION: We are in a zone thread.
	 * ASSERTION: We holding a search_pending_counter or the read_threads_mutex.
	 */
	int queue_index = -1;

	get_page_and_index(cache, physical_page, &queue_index, page);
}

static int read_page_locked(struct volume *volume, u32 physical_page,
			    struct cached_page **page_ptr)
{
	int result = UDS_SUCCESS;
	struct cached_page *page = NULL;
	u8 *page_data;

	page = select_victim_in_cache(&volume->page_cache);
	page_data = dm_bufio_read(volume->client, physical_page, &page->buffer);
	if (IS_ERR(page_data)) {
		result = -PTR_ERR(page_data);
		uds_log_warning_strerror(result,
					 "error reading physical page %u from volume",
					 physical_page);
		cancel_page_in_cache(&volume->page_cache, physical_page, page);
		return result;
	}

	if (!is_record_page(volume->geometry, physical_page)) {
		result = initialize_index_page(volume, physical_page, page);
		if (result != UDS_SUCCESS) {
			if (volume->lookup_mode != LOOKUP_FOR_REBUILD)
				uds_log_warning("Corrupt index page %u", physical_page);
			cancel_page_in_cache(&volume->page_cache, physical_page, page);
			return result;
		}
	}

	result = put_page_in_cache(&volume->page_cache, physical_page, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", physical_page);
		cancel_page_in_cache(&volume->page_cache, physical_page, page);
		return result;
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

/* Retrieve a page from the cache while holding the read threads mutex. */
STATIC int get_volume_page_locked(struct volume *volume, u32 physical_page,
				  struct cached_page **page_ptr)
{
	int result;
	struct cached_page *page = NULL;

	get_page_from_cache(&volume->page_cache, physical_page, &page);
	if (page == NULL) {
		result = read_page_locked(volume, physical_page, &page);
		if (result != UDS_SUCCESS)
			return result;
	} else {
		make_page_most_recent(&volume->page_cache, page);
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

/* Retrieve a page from the cache while holding a search_pending lock. */
STATIC int get_volume_page_protected(struct volume *volume,
				     struct uds_request *request,
				     u32 physical_page,
				     struct cached_page **page_ptr)
{
	struct cached_page *page;

	get_page_from_cache(&volume->page_cache, physical_page, &page);
	if (page != NULL) {
		if (request->zone_number == 0)
			/* Only one zone is allowed to update the LRU. */
			make_page_most_recent(&volume->page_cache, page);

		*page_ptr = page;
		return UDS_SUCCESS;
	}

	/* Prepare to enqueue a read for the page. */
	end_pending_search(&volume->page_cache, request->zone_number);
	uds_lock_mutex(&volume->read_threads_mutex);

	/*
	 * Do the lookup again while holding the read mutex (no longer the fast case so this should
	 * be fine to repeat). We need to do this because a page may have been added to the cache
	 * by a reader thread between the time we searched above and the time we went to actually
	 * try to enqueue it below. This could result in us enqueuing another read for a page which
	 * is already in the cache, which would mean we end up with two entries in the cache for
	 * the same page.
	 */
	get_page_from_cache(&volume->page_cache, physical_page, &page);
	if (page == NULL) {
		enqueue_page_read(volume, request, physical_page);
		/*
		 * The performance gain from unlocking first, while "search pending" mode is off,
		 * turns out to be significant in some cases. The page is not available yet so
		 * the order does not matter for correctness as it does below.
		 */
		uds_unlock_mutex(&volume->read_threads_mutex);
		begin_pending_search(&volume->page_cache, physical_page, request->zone_number);
		return UDS_QUEUED;
	}

	/*
	 * Now that the page is loaded, the volume needs to switch to "reader thread unlocked" and
	 * "search pending" state in careful order so no other thread can mess with the data before
	 * the caller gets to look at it.
	 */
	begin_pending_search(&volume->page_cache, physical_page, request->zone_number);
	uds_unlock_mutex(&volume->read_threads_mutex);
	*page_ptr = page;
	return UDS_SUCCESS;
}

static int get_volume_page(struct volume *volume, u32 chapter, u32 page_number,
			   struct cached_page **page_ptr)
{
	int result;
	u32 physical_page = map_to_physical_page(volume->geometry, chapter, page_number);

	uds_lock_mutex(&volume->read_threads_mutex);
	result = get_volume_page_locked(volume, physical_page, page_ptr);
	uds_unlock_mutex(&volume->read_threads_mutex);
	return result;
}

int uds_get_volume_record_page(struct volume *volume, u32 chapter,
			       u32 page_number, u8 **data_ptr)
{
	int result;
	struct cached_page *page = NULL;

	result = get_volume_page(volume, chapter, page_number, &page);
	if (result == UDS_SUCCESS)
		*data_ptr = dm_bufio_get_block_data(page->buffer);
	return result;
}

int uds_get_volume_index_page(struct volume *volume, u32 chapter,
			      u32 page_number,
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
				    struct uds_request *request, u32 chapter,
				    u32 index_page_number,
				    u16 *record_page_number)
{
	int result;
	struct cached_page *page = NULL;
	u32 physical_page = map_to_physical_page(volume->geometry, chapter, index_page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read the mapping. This
	 * prevents this thread from reading a page in the cache which has already been marked for
	 * invalidation by the reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(&volume->page_cache, physical_page, request->zone_number);

	result = get_volume_page_protected(volume, request, physical_page, &page);
	if (result != UDS_SUCCESS) {
		end_pending_search(&volume->page_cache, request->zone_number);
		return result;
	}

	result = uds_search_chapter_index_page(&page->index_page,
					       volume->geometry,
					       &request->record_name,
					       record_page_number);
	end_pending_search(&volume->page_cache, request->zone_number);
	return result;
}

/*
 * Find the metadata associated with a name in a given record page. This will return UDS_QUEUED if
 * the page in question must be read from storage.
 */
int uds_search_cached_record_page(struct volume *volume,
				  struct uds_request *request, u32 chapter,
				  u16 record_page_number, bool *found)
{
	struct cached_page *record_page;
	struct geometry *geometry = volume->geometry;
	int result;
	u32 physical_page, page_number;

	*found = false;
	if (record_page_number == NO_CHAPTER_INDEX_ENTRY)
		return UDS_SUCCESS;

	result = ASSERT(record_page_number < geometry->record_pages_per_chapter,
			"0 <= %d < %u",
			record_page_number,
			geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS)
		return result;

	page_number = geometry->index_pages_per_chapter + record_page_number;

	physical_page = map_to_physical_page(volume->geometry, chapter, page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read the mapping. This
	 * prevents this thread from reading a page in the cache which has already been marked for
	 * invalidation by the reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(&volume->page_cache, physical_page, request->zone_number);

	result = get_volume_page_protected(volume, request, physical_page, &record_page);
	if (result != UDS_SUCCESS) {
		end_pending_search(&volume->page_cache, request->zone_number);
		return result;
	}

	if (search_record_page(dm_bufio_get_block_data(record_page->buffer),
			       &request->record_name,
			       geometry,
			       &request->old_metadata))
		*found = true;

	end_pending_search(&volume->page_cache, request->zone_number);
	return UDS_SUCCESS;
}

void uds_prefetch_volume_chapter(const struct volume *volume, u32 chapter)
{
	const struct geometry *geometry = volume->geometry;
	u32 physical_page = map_to_physical_page(geometry, chapter, 0);

	dm_bufio_prefetch(volume->client, physical_page, geometry->pages_per_chapter);
}

int uds_read_chapter_index_from_volume(const struct volume *volume,
				       u64 virtual_chapter,
				       struct dm_buffer *volume_buffers[],
				       struct delta_index_page index_pages[])
{
	int result;
	u32 i;
	const struct geometry *geometry = volume->geometry;
	u32 physical_chapter = uds_map_to_physical_chapter(geometry, virtual_chapter);
	u32 physical_page = map_to_physical_page(geometry, physical_chapter, 0);

	dm_bufio_prefetch(volume->client, physical_page,
			  geometry->index_pages_per_chapter);
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

int uds_search_volume_page_cache(struct volume *volume,
				 struct uds_request *request, bool *found)
{
	int result;
	u32 physical_chapter =
		uds_map_to_physical_chapter(volume->geometry, request->virtual_chapter);
	u32 index_page_number;
	u16 record_page_number;

	index_page_number = uds_find_index_page_number(volume->index_page_map,
						       &request->record_name,
						       physical_chapter);

	if (request->location == UDS_LOCATION_INDEX_PAGE_LOOKUP) {
		record_page_number = *((u16 *) &request->old_metadata);
	} else {
		result = search_cached_index_page(volume,
						  request,
						  physical_chapter,
						  index_page_number,
						  &record_page_number);
		if (result != UDS_SUCCESS)
			return result;
	}

	return uds_search_cached_record_page(volume,
					     request,
					     physical_chapter,
					     record_page_number,
					     found);
}

int uds_search_volume_page_cache_for_rebuild(struct volume *volume,
					     const struct uds_record_name *name,
					     u64 virtual_chapter, bool *found)
{
	int result;
	struct geometry *geometry = volume->geometry;
	struct cached_page *page;
	u32 physical_chapter = uds_map_to_physical_chapter(geometry, virtual_chapter);
	u32 index_page_number;
	u16 record_page_number;
	u32 page_number;

	*found = false;
	index_page_number =
		uds_find_index_page_number(volume->index_page_map, name, physical_chapter);
	result = get_volume_page(volume, physical_chapter, index_page_number, &page);
	if (result != UDS_SUCCESS)
		return result;

	result = uds_search_chapter_index_page(&page->index_page,
					       geometry,
					       name,
					       &record_page_number);
	if (result != UDS_SUCCESS)
		return result;

	if (record_page_number == NO_CHAPTER_INDEX_ENTRY)
		return UDS_SUCCESS;

	page_number = geometry->index_pages_per_chapter + record_page_number;
	result = get_volume_page(volume, physical_chapter, page_number, &page);
	if (result != UDS_SUCCESS)
		return result;

	*found = search_record_page(dm_bufio_get_block_data(page->buffer), name, geometry, NULL);
	return UDS_SUCCESS;
}

STATIC void invalidate_page(struct page_cache *cache, u32 physical_page)
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

void uds_forget_chapter(struct volume *volume, u64 virtual_chapter)
{
	u32 physical_chapter = uds_map_to_physical_chapter(volume->geometry, virtual_chapter);
	u32 first_page = map_to_physical_page(volume->geometry, physical_chapter, 0);
	u32 i;

	uds_log_debug("forgetting chapter %llu", (unsigned long long) virtual_chapter);
	uds_lock_mutex(&volume->read_threads_mutex);
	for (i = 0; i < volume->geometry->pages_per_chapter; i++)
		invalidate_page(&volume->page_cache, first_page + i);
	uds_unlock_mutex(&volume->read_threads_mutex);
}

/*
 * Donate an index pages from a newly written chapter to the page cache since it is likely to be
 * used again soon. The caller must already hold the reader thread mutex.
 */
static int donate_index_page_locked(struct volume *volume,
				    u32 physical_chapter,
				    u32 index_page_number,
				    struct dm_buffer *page_buffer)
{
	int result;
	struct cached_page *page = NULL;
	u32 physical_page =
		map_to_physical_page(volume->geometry, physical_chapter, index_page_number);

	page = select_victim_in_cache(&volume->page_cache);
	page->buffer = page_buffer;
	result = init_chapter_index_page(volume,
					 dm_bufio_get_block_data(page_buffer),
					 physical_chapter,
					 index_page_number,
					 &page->index_page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error initialize chapter index page");
		cancel_page_in_cache(&volume->page_cache, physical_page, page);
		return result;
	}

	result = put_page_in_cache(&volume->page_cache, physical_page, page);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error putting page %u in cache", physical_page);
		cancel_page_in_cache(&volume->page_cache, physical_page, page);
		return result;
	}

	return UDS_SUCCESS;
}

static int write_index_pages(struct volume *volume,
			     u32 physical_chapter_number,
			     struct open_chapter_index *chapter_index)
{
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	u32 first_index_page = map_to_physical_page(geometry, physical_chapter_number, 0);
	u32 delta_list_number = 0;
	u32 index_page_number;

	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		u8 *page_data;
		u32 physical_page = first_index_page + index_page_number;
		u32 lists_packed;
		bool last_page;
		int result;

		page_data = dm_bufio_new(volume->client, physical_page, &page_buffer);
		if (IS_ERR(page_data))
			return uds_log_warning_strerror(-PTR_ERR(page_data),
							"failed to prepare index page");

		last_page = ((index_page_number + 1) == geometry->index_pages_per_chapter);
		result = uds_pack_open_chapter_index_page(chapter_index,
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

		if (physical_page < test_page_count)
			memcpy(test_pages[physical_page], page_data, geometry->bytes_per_page);

#endif /* TEST_INTERNAL */
		dm_bufio_mark_buffer_dirty(page_buffer);

		if (lists_packed == 0)
			uds_log_debug("no delta lists packed on chapter %u page %u",
				      physical_chapter_number,
				      index_page_number);
		else
			delta_list_number += lists_packed;

		uds_update_index_page_map(volume->index_page_map,
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

static u32 encode_tree(u8 record_page[],
		       const struct uds_volume_record *sorted_pointers[],
		       u32 next_record, u32 node, u32 node_count)
{
	if (node < node_count) {
		u32 child = (2 * node) + 1;

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

STATIC int encode_record_page(const struct volume *volume,
			      const struct uds_volume_record records[],
			      u8 record_page[])
{
	int result;
	u32 i;
	u32 records_per_page = volume->geometry->records_per_page;
	const struct uds_volume_record **record_pointers = volume->record_pointers;

	for (i = 0; i < records_per_page; i++)
		record_pointers[i] = &records[i];

	/*
	 * Sort the record pointers by using just the names in the records, which is less work than
	 * sorting the entire record values.
	 */
	BUILD_BUG_ON(offsetof(struct uds_volume_record, name) != 0);
	result = uds_radix_sort(volume->radix_sorter,
				(const u8 **) record_pointers,
				records_per_page,
				UDS_RECORD_NAME_SIZE);
	if (result != UDS_SUCCESS)
		return result;

	encode_tree(record_page, record_pointers, 0, 0, records_per_page);
	return UDS_SUCCESS;
}

static int write_record_pages(struct volume *volume,
			      u32 physical_chapter_number,
			      const struct uds_volume_record *records)
{
	u32 record_page_number;
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	const struct uds_volume_record *next_record = records;
	u32 first_record_page = map_to_physical_page(geometry,
						     physical_chapter_number,
						     geometry->index_pages_per_chapter);

	for (record_page_number = 0;
	     record_page_number < geometry->record_pages_per_chapter;
	     record_page_number++) {
		u8 *page_data;
		u32 physical_page = first_record_page + record_page_number;
		int result;

		page_data = dm_bufio_new(volume->client, physical_page, &page_buffer);
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

		if (physical_page < test_page_count)
			memcpy(test_pages[physical_page], page_data, geometry->bytes_per_page);

#endif /* TEST_INTERNAL */
		dm_bufio_mark_buffer_dirty(page_buffer);
		dm_bufio_release(page_buffer);
	}

	return UDS_SUCCESS;
}

int uds_write_chapter(struct volume *volume,
		      struct open_chapter_index *chapter_index,
		      const struct uds_volume_record *records)
{
	int result;
	u32 physical_chapter_number =
		uds_map_to_physical_chapter(volume->geometry,
					    chapter_index->virtual_chapter_number);

	result = write_index_pages(volume, physical_chapter_number, chapter_index);
	if (result != UDS_SUCCESS)
		return result;

	result = write_record_pages(volume, physical_chapter_number, records);
	if (result != UDS_SUCCESS)
		return result;

	result = -dm_bufio_write_dirty_buffers(volume->client);
	if (result != UDS_SUCCESS)
		uds_log_error_strerror(result, "cannot sync chapter to volume");

	return result;
}

static void probe_chapter(struct volume *volume, u32 chapter_number,
			  u64 *virtual_chapter_number)
{
	const struct geometry *geometry = volume->geometry;
	u32 expected_list_number = 0;
	u32 i;
	u64 vcn = BAD_CHAPTER;

#ifdef TEST_INTERNAL
	if (chapter_tester != NULL) {
		chapter_tester(chapter_number, virtual_chapter_number);
		return;
	}

#endif /* TEST_INTERNAL*/
	*virtual_chapter_number = BAD_CHAPTER;
	dm_bufio_prefetch(volume->client,
			  map_to_physical_page(geometry, chapter_number, 0),
			  geometry->index_pages_per_chapter);

	for (i = 0; i < geometry->index_pages_per_chapter; ++i) {
		struct delta_index_page *page;
		int result;

		result = uds_get_volume_index_page(volume, chapter_number, i, &page);
		if (result != UDS_SUCCESS)
			return;

		if (page->virtual_chapter_number == BAD_CHAPTER) {
			uds_log_error("corrupt index page in chapter %u", chapter_number);
			return;
		}

		if (vcn == BAD_CHAPTER) {
			vcn = page->virtual_chapter_number;
		} else if (page->virtual_chapter_number != vcn) {
			uds_log_error("inconsistent chapter %u index page %u: expected vcn %llu, got vcn %llu",
				      chapter_number,
				      i,
				      (unsigned long long) vcn,
				      (unsigned long long) page->virtual_chapter_number);
			return;
		}

		if (expected_list_number != page->lowest_list_number) {
			uds_log_error("inconsistent chapter %u index page %u: expected list number %u, got list number %u",
				      chapter_number,
				      i,
				      expected_list_number,
				      page->lowest_list_number);
			return;
		}
		expected_list_number = page->highest_list_number + 1;

		result = uds_validate_chapter_index_page(page, geometry);
		if (result != UDS_SUCCESS)
			return;
	}

	if (chapter_number != uds_map_to_physical_chapter(geometry, vcn)) {
		uds_log_error("chapter %u vcn %llu is out of phase (%u)",
			      chapter_number,
			      (unsigned long long) vcn,
			      geometry->chapters_per_volume);
		return;
	}

	*virtual_chapter_number = vcn;
}

/* Find the last valid physical chapter in the volume. */
static void find_real_end_of_volume(struct volume *volume, u32 limit,
				    u32 *limit_ptr)
{
	u32 span = 1;
	u32 tries = 0;

	while (limit > 0) {
		u32 chapter = (span > limit) ? 0 : limit - span;
		u64 vcn = 0;

		probe_chapter(volume, chapter, &vcn);
		if (vcn == BAD_CHAPTER) {
			limit = chapter;
			if (++tries > 1)
				span *= 2;
		} else {
			if (span == 1)
				break;
			span /= 2;
			tries = 0;
		}
	}

	*limit_ptr = limit;
}

STATIC int find_chapter_limits(struct volume *volume, u32 chapter_limit,
			       u64 *lowest_vcn, u64 *highest_vcn)
{
	struct geometry *geometry = volume->geometry;
	u64 zero_vcn;
	u64 lowest = BAD_CHAPTER;
	u64 highest = BAD_CHAPTER;
	u64 moved_chapter = BAD_CHAPTER;
	u32 left_chapter = 0;
	u32 right_chapter = 0;
	u32 bad_chapters = 0;

	/*
	 * This method assumes there is at most one run of contiguous bad chapters caused by
	 * unflushed writes. Either the bad spot is at the beginning and end, or somewhere in the
	 * middle. Wherever it is, the highest and lowest VCNs are adjacent to it. Otherwise the
	 * volume is cleanly saved and somewhere in the middle of it the highest VCN immediately
	 * precedes the lowest one.
	 */

	/* It doesn't matter if this results in a bad spot (BAD_CHAPTER). */
	probe_chapter(volume, 0, &zero_vcn);

	/*
	 * Binary search for end of the discontinuity in the monotonically increasing virtual
	 * chapter numbers; bad spots are treated as a span of BAD_CHAPTER values. In effect we're
	 * searching for the index of the smallest value less than zero_vcn. In the case we go off
	 * the end it means that chapter 0 has the lowest vcn.
	 *
	 * If a virtual chapter is out-of-order, it will be the one moved by conversion. Always
	 * skip over the moved chapter when searching, adding it to the range at the end if
	 * necessary.
	 */
	if (geometry->remapped_physical > 0) {
		u64 remapped_vcn;

		probe_chapter(volume, geometry->remapped_physical, &remapped_vcn);
		if (remapped_vcn == geometry->remapped_virtual)
			moved_chapter = geometry->remapped_physical;
	}

	left_chapter = 0;
	right_chapter = chapter_limit;

	while (left_chapter < right_chapter) {
		u64 probe_vcn;
		u32 chapter = (left_chapter + right_chapter) / 2;

		if (chapter == moved_chapter)
			chapter--;

		probe_chapter(volume, chapter, &probe_vcn);
		if (zero_vcn <= probe_vcn) {
			left_chapter = chapter + 1;
			if (left_chapter == moved_chapter)
				left_chapter++;
		} else {
			right_chapter = chapter;
		}
	}

	/* If left_chapter goes off the end, chapter 0 has the lowest virtual chapter number.*/
	if (left_chapter >= chapter_limit)
		left_chapter = 0;

	/* At this point, left_chapter is the chapter with the lowest virtual chapter number. */
	probe_chapter(volume, left_chapter, &lowest);

	/* The moved chapter might be the lowest in the range. */
	if ((moved_chapter != BAD_CHAPTER) && (lowest == geometry->remapped_virtual + 1))
		lowest = geometry->remapped_virtual;

	/*
	 * Circularly scan backwards, moving over any bad chapters until encountering a good one,
	 * which is the chapter with the highest vcn.
	 */
	while (highest == BAD_CHAPTER) {
		right_chapter = (right_chapter + chapter_limit - 1) % chapter_limit;
		if (right_chapter == moved_chapter)
			continue;

		probe_chapter(volume, right_chapter, &highest);
		if (bad_chapters++ >= MAX_BAD_CHAPTERS) {
			uds_log_error("too many bad chapters in volume: %u", bad_chapters);
			return UDS_CORRUPT_DATA;
		}
	}

	*lowest_vcn = lowest;
	*highest_vcn = highest;
	return UDS_SUCCESS;
}

/*
 * Find the highest and lowest contiguous chapters present in the volume and determine their
 * virtual chapter numbers. This is used by rebuild.
 */
int uds_find_volume_chapter_boundaries(struct volume *volume, u64 *lowest_vcn,
				       u64 *highest_vcn, bool *is_empty)
{
	u32 chapter_limit = volume->geometry->chapters_per_volume;

	find_real_end_of_volume(volume, chapter_limit, &chapter_limit);
	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		*is_empty = true;
		return UDS_SUCCESS;
	}

	*is_empty = false;
	return find_chapter_limits(volume, chapter_limit, lowest_vcn, highest_vcn);
}

int __must_check uds_replace_volume_storage(struct volume *volume,
					    struct index_layout *layout,
					    struct block_device *bdev)
{
	int result;
	u32 i;

	result = uds_replace_index_layout_storage(layout, bdev);
	if (result != UDS_SUCCESS)
		return result;

	/* Release all outstanding dm_bufio objects */
	for (i = 0; i < volume->page_cache.indexable_pages; i++)
		volume->page_cache.index[i] = volume->page_cache.cache_slots;
	for (i = 0; i < volume->page_cache.cache_slots; i++)
		clear_cache_page(&volume->page_cache, &volume->page_cache.cache[i]);
	if (volume->sparse_cache != NULL)
		uds_invalidate_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL)
		dm_bufio_client_destroy(UDS_FORGET(volume->client));

	return uds_open_volume_bufio(layout,
				     volume->geometry->bytes_per_page,
				     volume->reserved_buffers,
				     &volume->client);
}

STATIC int __must_check initialize_page_cache(struct page_cache *cache,
					      const struct geometry *geometry,
					      u32 chapters_in_cache,
					      unsigned int zone_count)
{
	int result;
	u32 i;

	cache->indexable_pages = geometry->pages_per_volume + 1;
	cache->cache_slots = chapters_in_cache * geometry->record_pages_per_chapter;
	cache->zone_count = zone_count;
	atomic64_set(&cache->clock, 1);

	result = ASSERT((cache->cache_slots <= VOLUME_CACHE_MAX_ENTRIES),
			"requested cache size, %u, within limit %u",
			cache->cache_slots,
			VOLUME_CACHE_MAX_ENTRIES);
	if (result != UDS_SUCCESS)
		return result;

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

	result = UDS_ALLOCATE(cache->indexable_pages, u16, "page cache index", &cache->index);
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE(cache->cache_slots,
			      struct cached_page,
			      "page cache cache",
			      &cache->cache);
	if (result != UDS_SUCCESS)
		return result;

	/* Initialize index values to invalid values. */
	for (i = 0; i < cache->indexable_pages; i++)
		cache->index[i] = cache->cache_slots;

	for (i = 0; i < cache->cache_slots; i++)
		clear_cache_page(cache, &cache->cache[i]);

	return UDS_SUCCESS;
}

int uds_make_volume(const struct configuration *config,
		    struct index_layout *layout, struct volume **new_volume)
{
	unsigned int i;
	struct volume *volume = NULL;
	struct geometry *geometry;
	unsigned int reserved_buffers;
	int result;

	result = UDS_ALLOCATE(1, struct volume, "volume", &volume);
	if (result != UDS_SUCCESS)
		return result;

	volume->nonce = uds_get_volume_nonce(layout);

	result = uds_copy_geometry(config->geometry, &volume->geometry);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return uds_log_warning_strerror(result, "failed to allocate geometry: error");
	}
	geometry = volume->geometry;

	/*
	 * Reserve a buffer for each entry in the page cache, one for the chapter writer, and one
	 * for each entry in the sparse cache.
	 */
	reserved_buffers = config->cache_chapters * geometry->record_pages_per_chapter;
	reserved_buffers += 1;
	if (uds_is_sparse_geometry(geometry))
		reserved_buffers += (config->cache_chapters * geometry->index_pages_per_chapter);
	volume->reserved_buffers = reserved_buffers;
	result = uds_open_volume_bufio(layout,
				       geometry->bytes_per_page,
				       volume->reserved_buffers,
				       &volume->client);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = uds_make_radix_sorter(geometry->records_per_page, &volume->radix_sorter);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = UDS_ALLOCATE(geometry->records_per_page,
			      const struct uds_volume_record *,
			      "record pointers",
			      &volume->record_pointers);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	if (uds_is_sparse_geometry(geometry)) {
		size_t page_size = sizeof(struct delta_index_page) + geometry->bytes_per_page;

		result = uds_make_sparse_cache(geometry,
					       config->cache_chapters,
					       config->zone_count,
					       &volume->sparse_cache);
		if (result != UDS_SUCCESS) {
			uds_free_volume(volume);
			return result;
		}

		volume->cache_size =
			page_size * geometry->index_pages_per_chapter * config->cache_chapters;
	}

	result = initialize_page_cache(&volume->page_cache,
				       geometry,
				       config->cache_chapters,
				       config->zone_count);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	volume->cache_size += volume->page_cache.cache_slots * sizeof(struct delta_index_page);
	result = uds_make_index_page_map(geometry, &volume->index_page_map);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = uds_init_mutex(&volume->read_threads_mutex);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = uds_init_cond(&volume->read_threads_read_done_cond);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = uds_init_cond(&volume->read_threads_cond);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	result = UDS_ALLOCATE(config->read_threads,
			      struct thread *,
			      "reader threads",
			      &volume->reader_threads);
	if (result != UDS_SUCCESS) {
		uds_free_volume(volume);
		return result;
	}

	for (i = 0; i < config->read_threads; i++) {
		result = uds_create_thread(read_thread_function,
					   (void *) volume,
					   "reader",
					   &volume->reader_threads[i]);
		if (result != UDS_SUCCESS) {
			uds_free_volume(volume);
			return result;
		}

		volume->read_thread_count = i + 1;
	}

	*new_volume = volume;
	return UDS_SUCCESS;
}

STATIC void uninitialize_page_cache(struct page_cache *cache)
{
	u16 i;

	if (cache->cache != NULL) {
		for (i = 0; i < cache->cache_slots; i++)
			release_page_buffer(&cache->cache[i]);
	}
	UDS_FREE(cache->index);
	UDS_FREE(cache->cache);
	UDS_FREE(cache->search_pending_counters);
	UDS_FREE(cache->read_queue);
}

void uds_free_volume(struct volume *volume)
{
	if (volume == NULL)
		return;

	if (volume->reader_threads != NULL) {
		unsigned int i;

		/* This works even if some threads weren't started. */
		uds_lock_mutex(&volume->read_threads_mutex);
		volume->read_threads_exiting = true;
		uds_broadcast_cond(&volume->read_threads_cond);
		uds_unlock_mutex(&volume->read_threads_mutex);
		for (i = 0; i < volume->read_thread_count; i++)
			uds_join_threads(volume->reader_threads[i]);
		UDS_FREE(volume->reader_threads);
		volume->reader_threads = NULL;
	}

	/* Must destroy the client AFTER freeing the cached pages. */
	uninitialize_page_cache(&volume->page_cache);
	uds_free_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL)
		dm_bufio_client_destroy(UDS_FORGET(volume->client));

	uds_destroy_cond(&volume->read_threads_cond);
	uds_destroy_cond(&volume->read_threads_read_done_cond);
	uds_destroy_mutex(&volume->read_threads_mutex);
	uds_free_index_page_map(volume->index_page_map);
	uds_free_radix_sorter(volume->radix_sorter);
	UDS_FREE(volume->geometry);
	UDS_FREE(volume->record_pointers);
	UDS_FREE(volume);
}
