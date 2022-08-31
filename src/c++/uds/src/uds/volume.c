// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "volume.h"

#include <linux/atomic.h>
#include <linux/dm-bufio.h>

#include "chapter-index.h"
#include "compiler.h"
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

enum {
	MAX_BAD_CHAPTERS = 100,  /* max number of contiguous bad chapters */
};

#ifdef TEST_INTERNAL
/*
 * This function pointer allows unit test code to intercept the slow-lane
 * requeuing of a request.
 */
static request_restarter_t request_restarter = NULL;

void set_request_restarter(request_restarter_t restarter)
{
	request_restarter = restarter;
}

#endif /* TEST_INTERNAL */
static INLINE unsigned int map_to_page_number(struct geometry *geometry,
					      unsigned int physical_page)
{
	return ((physical_page - 1) % geometry->pages_per_chapter);
}

static INLINE unsigned int map_to_chapter_number(struct geometry *geometry,
						 unsigned int physical_page)
{
	return ((physical_page - 1) / geometry->pages_per_chapter);
}

static INLINE bool is_record_page(struct geometry *geometry,
				  unsigned int physical_page)
{
	return (((physical_page - 1) % geometry->pages_per_chapter) >=
		geometry->index_pages_per_chapter);
}

static INLINE unsigned int get_zone_number(struct uds_request *request)
{
	return (request == NULL) ? 0 : request->zone_number;
}

int map_to_physical_page(const struct geometry *geometry,
			 int chapter,
			 int page)
{
	/*
	 * Page zero is the header page, so the first index page in the
	 * first chapter is physical page one.
	 */
	return (1 + (geometry->pages_per_chapter * chapter) + page);
}

int assert_page_in_cache(struct page_cache *cache, struct cached_page *page)
{
	uint16_t page_index;
	int result = ASSERT((page->cp_physical_page < cache->num_index_entries),
			    "physical page %u is valid (< %u)",
			    page->cp_physical_page,
			    cache->num_index_entries);
	if (result != UDS_SUCCESS) {
		return result;
	}

	page_index = cache->index[page->cp_physical_page];
	return ASSERT((page_index < cache->num_cache_entries) &&
			      (&cache->cache[page_index] == page),
		      "page is at expected location in cache");
}

static void release_page_buffer(struct cached_page *page)
{
	if (page->buffer != NULL) {
		dm_bufio_release(UDS_FORGET(page->buffer));
	}
}

/**
 * Clear a cache page. Note: this does not clear read_pending - a read could
 * still be pending and the read thread needs to be able to proceed and restart
 * the requests regardless. This page will still be marked invalid, but it
 * won't get reused (see get_least_recent_page()) until the read_pending flag
 * is cleared. This is a valid case, e.g. the chapter gets forgotten and
 * replaced with a new one in LRU. Restarting the requests will lead them to
 * not find the records in the MI.
 *
 * @param cache   the cache
 * @param page    the cached page to clear
 *
 **/
static void clear_cache_page(struct page_cache *cache,
			     struct cached_page *page)
{
	release_page_buffer(page);
	page->cp_physical_page = cache->num_index_entries;
	WRITE_ONCE(page->cp_last_used, 0);
}

/**
 * Get a page from the cache, but with no stats
 *
 * @param cache         the cache
 * @param physical_page the physical page to get
 * @param queue_index   the index of the page in the read queue if
 *                      queued, -1 otherwise
 * @param page_ptr      a pointer to hold the page
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check get_page_and_index(struct page_cache *cache,
					   unsigned int physical_page,
					   int *queue_index,
					   struct cached_page **page_ptr)
{
	uint16_t index_value, index;
	bool queued;

	/*
	 * ASSERTION: We are either a zone thread holding a
	 * search_pending_counter, or we are any thread holding the
	 * readThreadsMutex.
	 *
	 * Holding only a search_pending_counter is the most frequent case.
	 */

	int result = ASSERT((physical_page < cache->num_index_entries),
			    "physical page %u is invalid",
			    physical_page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * It would be unlikely that the compiler turns the usage of
	 * index_value into two reads of cache->index, but it would be
	 * possible and very bad if those reads did not return the
	 * same bits.
	 */
	index_value = READ_ONCE(cache->index[physical_page]);
	queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;
	index = index_value & ~VOLUME_CACHE_QUEUED_FLAG;

	if (!queued && (index < cache->num_cache_entries)) {
		*page_ptr = &cache->cache[index];
		/*
		 * We have acquired access to the cached page, but
		 * unless we hold the readThreadsMutex, we need a read
		 * memory barrier now. The corresponding write memory
		 * barrier is in put_page_in_cache.
		 */
		smp_rmb();
	} else {
		*page_ptr = NULL;
	}

	*queue_index = queued ? index : -1;
	return UDS_SUCCESS;
}

/**
 * Wait for all pending searches on a page in the cache to complete
 *
 * @param cache          the page cache
 * @param physical_page  the page to check searches on
 **/
static void wait_for_pending_searches(struct page_cache *cache,
				      unsigned int physical_page)
{
	invalidate_counter_t initial_counters[MAX_ZONES];
	unsigned int i;
	/*
	 * We hold the readThreadsMutex. We are waiting for threads
	 * that do not hold the readThreadsMutex. Those threads have
	 * "locked" their targeted page by setting the
	 * search_pending_counter. The corresponding write memory
	 * barrier is in begin_pending_search.
	 */
	smp_mb();

	for (i = 0; i < cache->zone_count; i++) {
		initial_counters[i] = get_invalidate_counter(cache, i);
	}
	for (i = 0; i < cache->zone_count; i++) {
		if (search_pending(initial_counters[i]) &&
		    (page_being_searched(initial_counters[i]) ==
		     physical_page)) {
			/*
			 * There is an active search using the physical page.
			 * We need to wait for the search to finish.
			 * FIXME: Investigate using wait_event() to
			 * wait for the search to finish.
			 */
			while (initial_counters[i] ==
			       get_invalidate_counter(cache, i)) {
				cond_resched();
			}
		}
	}
}

/**
 * Invalidate a cache page
 *
 * @param cache  the cache
 * @param page   the cached page
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check invalidate_page_in_cache(struct page_cache *cache,
						 struct cached_page *page)
{
	int result;

	/* We hold the readThreadsMutex. */
	if (page == NULL) {
		return UDS_SUCCESS;
	}

	if (page->cp_physical_page != cache->num_index_entries) {
		result = assert_page_in_cache(cache, page);

		if (result != UDS_SUCCESS) {
			return result;
		}

		WRITE_ONCE(cache->index[page->cp_physical_page],
			   cache->num_cache_entries);
		wait_for_pending_searches(cache, page->cp_physical_page);
	}

	clear_cache_page(cache, page);

	return UDS_SUCCESS;
}

EXTERNAL_STATIC
int find_invalidate_and_make_least_recent(struct page_cache *cache,
					  unsigned int physical_page,
					  bool must_find)
{
	struct cached_page *page;
	int queue_index = -1;
	int result;

	/* We hold the readThreadsMutex. */
	if (cache == NULL) {
		return UDS_SUCCESS;
	}

	result = get_page_and_index(cache, physical_page, &queue_index, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (page == NULL) {
		result = ASSERT(!must_find, "found page");
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (queue_index > -1) {
			uds_log_debug("setting pending read to invalid");
			cache->read_queue[queue_index].invalid = true;
		}
		return UDS_SUCCESS;
	}

	/* Invalidate the page and unmap it from the cache. */
	result = invalidate_page_in_cache(cache, page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Move the cached page to the least recently used end of the list
	 * so it will be replaced before any page with valid data.
	 */
	WRITE_ONCE(page->cp_last_used, 0);

	return UDS_SUCCESS;
}

static int __must_check initialize_page_cache(struct page_cache *cache,
					      const struct geometry *geometry,
					      unsigned int chapters_in_cache,
					      unsigned int zone_count)
{
	int result;
	unsigned int i;

	cache->geometry = geometry;
	cache->num_index_entries = geometry->pages_per_volume + 1;
	cache->num_cache_entries =
		chapters_in_cache * geometry->record_pages_per_chapter;
	cache->zone_count = zone_count;
	atomic64_set(&cache->clock, 1);

	result = UDS_ALLOCATE(VOLUME_CACHE_MAX_QUEUED_READS,
			      struct queued_read,
			      "volume read queue",
			      &cache->read_queue);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(cache->zone_count,
			      struct search_pending_counter,
			      "Volume Cache Zones",
			      &cache->search_pending_counters);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((cache->num_cache_entries <= VOLUME_CACHE_MAX_ENTRIES),
			"requested cache size, %u, within limit %u",
			cache->num_cache_entries,
			VOLUME_CACHE_MAX_ENTRIES);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(cache->num_index_entries,
			      uint16_t,
			      "page cache index",
			      &cache->index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Initialize index values to invalid values. */
	for (i = 0; i < cache->num_index_entries; i++) {
		cache->index[i] = cache->num_cache_entries;
	}

	result = UDS_ALLOCATE(cache->num_cache_entries,
			      struct cached_page,
			      "page cache cache",
			      &cache->cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < cache->num_cache_entries; i++) {
		clear_cache_page(cache, &cache->cache[i]);
	}

	return UDS_SUCCESS;
}

int make_page_cache(const struct geometry  *geometry,
		    unsigned int chapters_in_cache,
		    unsigned int zone_count,
		    struct page_cache **cache_ptr)
{
	struct page_cache *cache;
	int result;

	if (chapters_in_cache < 1) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cache size must be at least one chapter");
	}

	if (zone_count < 1) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cache must have at least one zone");
	}

	result = UDS_ALLOCATE(1, struct page_cache, "volume cache", &cache);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = initialize_page_cache(cache,
				       geometry,
				       chapters_in_cache,
				       zone_count);
	if (result != UDS_SUCCESS) {
		free_page_cache(cache);
		return result;
	}

	*cache_ptr = cache;
	return UDS_SUCCESS;
}

void free_page_cache(struct page_cache *cache)
{
	if (cache == NULL) {
		return;
	}
	if (cache->cache != NULL) {
		unsigned int i;

		for (i = 0; i < cache->num_cache_entries; i++) {
			release_page_buffer(&cache->cache[i]);
		}
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

	for (i = 0; i < cache->num_index_entries; i++) {
		cache->index[i] = cache->num_cache_entries;
	}

	for (i = 0; i < cache->num_cache_entries; i++) {
		clear_cache_page(cache, &cache->cache[i]);
	}
}

int invalidate_page_cache_for_chapter(struct page_cache *cache,
				      unsigned int chapter,
				      unsigned int pages_per_chapter)
{
	int result;
	unsigned int i;
	/* We hold the readThreadsMutex. */
	if ((cache == NULL) || (cache->cache == NULL)) {
		return UDS_SUCCESS;
	}

	for (i = 0; i < pages_per_chapter; i++) {
		unsigned int physical_page =
			1 + (pages_per_chapter * chapter) + i;
		result = find_invalidate_and_make_least_recent(cache,
							       physical_page,
							       false);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

void make_page_most_recent(struct page_cache *cache, struct cached_page *page)
{
	/*
	 * ASSERTION: We are either a zone thread holding a
	 * search_pending_counter, or we are any thread holding the
	 */
	/* readThreadsMutex. */
	if (atomic64_read(&cache->clock) != READ_ONCE(page->cp_last_used)) {
		WRITE_ONCE(page->cp_last_used,
			   atomic64_inc_return(&cache->clock));
	}
}

/**
 * Get the least recent valid page from the cache.
 *
 * @param cache    the cache
 * @param page_ptr  a pointer to hold the new page (will be set to NULL
 *                 if the page was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check get_least_recent_page(struct page_cache *cache,
					      struct cached_page **page_ptr)
{
	/* We hold the readThreadsMutex. */
	int oldest_index = 0;
	/*
	 * Our first candidate is any page that does have a pending read. We
	 * ensure above that there are more entries than read threads, so there
	 * must be one.
	 */
	unsigned int i;

	for (i = 0;; i++) {
		if (i >= cache->num_cache_entries) {
			/* This should never happen. */
			return ASSERT(false, "oldest page is not NULL");
		}
		if (!cache->cache[i].cp_read_pending) {
			oldest_index = i;
			break;
		}
	}
	/*
	 * Now find the least recently used page that does not have a pending
	 * read.
	 */
	for (i = 0; i < cache->num_cache_entries; i++) {
		if (!cache->cache[i].cp_read_pending &&
		    (READ_ONCE(cache->cache[i].cp_last_used) <=
		     READ_ONCE(cache->cache[oldest_index].cp_last_used))) {
			oldest_index = i;
		}
	}
	*page_ptr = &cache->cache[oldest_index];
	return UDS_SUCCESS;
}

int get_page_from_cache(struct page_cache *cache,
			unsigned int physical_page,
			struct cached_page **page)
{
	/*
	 * ASSERTION: We are in a zone thread.
	 * ASSERTION: We holding a search_pending_counter or the
	 * readThreadsMutex.
	 */
	int queue_index = -1;

	return get_page_and_index(cache, physical_page, &queue_index, page);
}

int select_victim_in_cache(struct page_cache *cache,
			   struct cached_page **page_ptr)
{
	struct cached_page *page = NULL;
	int result;
	/* We hold the readThreadsMutex. */
	if (cache == NULL) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cannot put page in NULL cache");
	}

	result = get_least_recent_page(cache, &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((page != NULL), "least recent page was not NULL");
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * If the page is currently being pointed to by the page map, clear
	 * it from the page map.
	 */
	if (page->cp_physical_page != cache->num_index_entries) {
		WRITE_ONCE(cache->index[page->cp_physical_page],
			   cache->num_cache_entries);
		wait_for_pending_searches(cache, page->cp_physical_page);
	}

	page->cp_read_pending = true;

	clear_cache_page(cache, page);
	*page_ptr = page;

	return UDS_SUCCESS;
}

int put_page_in_cache(struct page_cache *cache,
		      unsigned int physical_page,
		      struct cached_page *page)
{
	uint16_t value;
	int result;

	/* We hold the readThreadsMutex. */
	if (cache == NULL) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cannot complete page in NULL cache");
	}

	result = ASSERT((page != NULL), "page to install exists");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT((page->cp_read_pending),
			"page to install has a pending read");
	if (result != UDS_SUCCESS) {
		return result;
	}

	page->cp_physical_page = physical_page;

	/* Figure out the index into the cache array using pointer arithmetic */
	value = page - cache->cache;
	result = ASSERT((value < cache->num_cache_entries),
			"cache index is valid");
	if (result != UDS_SUCCESS) {
		return result;
	}

	make_page_most_recent(cache, page);

	page->cp_read_pending = false;

	/*
	 * We hold the readThreadsMutex, but we must have a write memory
	 * barrier before making the cached_page available to the readers
	 * that do not hold the mutex. The corresponding read memory
	 * barrier is in get_page_and_index().
	 */
	smp_wmb();

	/* Point the page map to the new page. Will clear queued flag */
	WRITE_ONCE(cache->index[physical_page], value);

	return UDS_SUCCESS;
}

void cancel_page_in_cache(struct page_cache *cache,
			  unsigned int physical_page,
			  struct cached_page *page)
{
	int result;
	/* We hold the readThreadsMutex. */
	if (cache == NULL) {
		uds_log_warning("cannot cancel page in NULL cache");
		return;
	}

	result = ASSERT((page != NULL), "page to install exists");
	if (result != UDS_SUCCESS) {
		return;
	}

	result = ASSERT((page->cp_read_pending),
			"page to install has a pending read");
	if (result != UDS_SUCCESS) {
		return;
	}

	clear_cache_page(cache, page);
	page->cp_read_pending = false;

	/* Clear the page map for the new page. Will clear queued flag */
	WRITE_ONCE(cache->index[physical_page], cache->num_cache_entries);
}

int enqueue_read(struct page_cache *cache,
		 struct uds_request *request,
		 unsigned int physical_page)
{
	int result;

	/* We hold the readThreadsMutex. */
	uint16_t first = cache->read_queue_first;
	uint16_t last = cache->read_queue_last;
	uint16_t next = next_read_queue_position(last);
	uint16_t read_queue_pos;

	if ((cache->index[physical_page] & VOLUME_CACHE_QUEUED_FLAG) == 0) {
		/* Not seen before, add this to the read queue and mark it as
		 * queued */
		if (next == first) {
			/* queue is full */
			return UDS_SUCCESS;
		}
		/* fill the read queue entry */
		cache->read_queue[last].physical_page = physical_page;
		cache->read_queue[last].invalid = false;

		/* point the cache index to it */
		read_queue_pos = last;
		WRITE_ONCE(cache->index[physical_page],
			   read_queue_pos | VOLUME_CACHE_QUEUED_FLAG);
		cache->read_queue[read_queue_pos].request_list.first = NULL;
		cache->read_queue[read_queue_pos].request_list.last = NULL;
		/* bump the last pointer */
		cache->read_queue_last = next;
	} else {
		/* It's already queued, just add on to it */
		read_queue_pos =
			cache->index[physical_page] & ~VOLUME_CACHE_QUEUED_FLAG;
	}

	result = ASSERT((read_queue_pos < VOLUME_CACHE_MAX_QUEUED_READS),
			    "queue is not overfull");
	if (result != UDS_SUCCESS) {
		return result;
	}

	request->next_request = NULL;
	if (cache->read_queue[read_queue_pos].request_list.first == NULL) {
		cache->read_queue[read_queue_pos].request_list.first = request;
	} else {
		cache->read_queue[read_queue_pos].request_list.last->next_request = request;
	}
	cache->read_queue[read_queue_pos].request_list.last = request;
	return UDS_QUEUED;
}

static void wait_for_read_queue_not_full(struct volume *volume,
					 struct uds_request *request)
{
	unsigned int zone_number = get_zone_number(request);
	invalidate_counter_t invalidate_counter =
		get_invalidate_counter(volume->page_cache, zone_number);

	if (search_pending(invalidate_counter)) {
		/*
		 * Increment the invalidate counter to avoid deadlock where the
		 * reader threads cannot make progress because they are waiting
		 * on the counter and the index thread cannot because the read
		 * queue is full.
		 */
		end_pending_search(volume->page_cache, zone_number);
	}

	while (read_queue_is_full(volume->page_cache)) {
		uds_log_debug("Waiting until read queue not full");
		uds_signal_cond(&volume->read_threads_cond);
		uds_wait_cond(&volume->read_threads_read_done_cond,
			      &volume->read_threads_mutex);
	}

	if (search_pending(invalidate_counter)) {
		/* Increment again so we get back to an odd value. */
		begin_pending_search(volume->page_cache,
				     page_being_searched(invalidate_counter),
				     zone_number);
	}
}

int enqueue_page_read(struct volume *volume,
		      struct uds_request *request,
		      int physical_page)
{
	int result;
	/*
	 * Don't allow new requests if we are shutting down, but make sure
	 * to process any requests that are still in the pipeline.
	 */
	if ((volume->reader_state & READER_STATE_EXIT) != 0) {
		uds_log_info("failed to queue read while shutting down");
		return -EBUSY;
	}

	/*
	 * Mark the page as queued in the volume cache, for chapter
	 * invalidation to be able to cancel a read. If we are unable to do
	 * this because the queues are full, flush them first
	 */
	while ((result = enqueue_read(volume->page_cache,
				      request,
				      physical_page)) == UDS_SUCCESS) {
		uds_log_debug("Read queues full, waiting for reads to finish");
		wait_for_read_queue_not_full(volume, request);
	}

	if (result == UDS_QUEUED) {
		/* signal a read thread */
		uds_signal_cond(&volume->read_threads_cond);
	}

	return result;
}

bool reserve_read_queue_entry(struct page_cache *cache,
			      unsigned int *queue_pos,
			      struct uds_request **first_request,
			      unsigned int *physical_page,
			      bool *invalid)
{
	/* We hold the readThreadsMutex. */
	uint16_t last_read = cache->read_queue_last_read;
	unsigned int page_no;
	uint16_t index_value;
	bool is_invalid, queued;

	/* No items to dequeue */
	if (last_read == cache->read_queue_last) {
		return false;
	}

	page_no = cache->read_queue[last_read].physical_page;
	is_invalid = cache->read_queue[last_read].invalid;

	index_value = cache->index[page_no];
	queued = (index_value & VOLUME_CACHE_QUEUED_FLAG) != 0;

	/*
	 * ALB-1429 ... need to check to see if its still queued before
	 * resetting
	 */
	if (is_invalid && queued) {
		/* invalidate cache index slot */
		WRITE_ONCE(cache->index[page_no], cache->num_cache_entries);
	}

	/*
	 * If a sync read has taken this page, set invalid to true so we don't
	 * overwrite, we simply just requeue requests.
	 */
	if (!queued) {
		is_invalid = true;
	}

	cache->read_queue[last_read].reserved = true;

	*queue_pos = last_read;
	*first_request = cache->read_queue[last_read].request_list.first;
	*physical_page = page_no;
	*invalid = is_invalid;
	cache->read_queue_last_read = next_read_queue_position(last_read);

	return true;
}

void release_read_queue_entry(struct page_cache *cache, unsigned int queue_pos)
{
	/* We hold the readThreadsMutex. */
	uint16_t last_read = cache->read_queue_last_read;

	cache->read_queue[queue_pos].reserved = false;

	/* Move the read_queue_first pointer along when we can */
	while ((cache->read_queue_first != last_read) &&
	       (!cache->read_queue[cache->read_queue_first].reserved)) {
		cache->read_queue_first =
			next_read_queue_position(cache->read_queue_first);
	}
}

static INLINE void
wait_to_reserve_read_queue_entry(struct volume *volume,
				 unsigned int *queue_pos,
				 struct uds_request **request_list,
				 unsigned int *physical_page,
				 bool *invalid)
{
	while (((volume->reader_state & READER_STATE_EXIT) == 0) &&
	       (((volume->reader_state & READER_STATE_STOP) != 0) ||
		!reserve_read_queue_entry(volume->page_cache,
					  queue_pos,
					  request_list,
					  physical_page,
					  invalid))) {
		uds_wait_cond(&volume->read_threads_cond,
			      &volume->read_threads_mutex);
	}
}

static int init_chapter_index_page(const struct volume *volume,
				   byte *index_page,
				   unsigned int chapter,
				   unsigned int index_page_number,
				   struct delta_index_page *chapter_index_page)
{
	uint64_t ci_virtual;
	unsigned int ci_chapter;
	unsigned int lowest_list;
	unsigned int highest_list;
	struct geometry *geometry = volume->geometry;

	int result = initialize_chapter_index_page(chapter_index_page,
						   geometry, index_page,
						   volume->nonce);
	if (volume->lookup_mode == LOOKUP_FOR_REBUILD) {
		return result;
	}
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "Reading chapter index page for chapter %u page %u",
					      chapter, index_page_number);
	}

	get_list_number_bounds(volume->index_page_map,
			       chapter,
			       index_page_number,
			       &lowest_list,
			       &highest_list);
	ci_virtual = chapter_index_page->virtual_chapter_number;
	ci_chapter = map_to_physical_chapter(geometry, ci_virtual);
	if ((chapter == ci_chapter) &&
	    (lowest_list == chapter_index_page->lowest_list_number) &&
	    (highest_list == chapter_index_page->highest_list_number)) {
		return UDS_SUCCESS;
	}

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
	unsigned int chapter =
		map_to_chapter_number(volume->geometry, physical_page);
	unsigned int index_page_number =
		map_to_page_number(volume->geometry, physical_page);
	int result =
		init_chapter_index_page(volume,
					dm_bufio_get_block_data(page->buffer),
					chapter,
					index_page_number,
					&page->cp_index_page);
	return result;
}

EXTERNAL_STATIC bool search_record_page(const byte record_page[],
					const struct uds_chunk_name *name,
					const struct geometry *geometry,
					struct uds_chunk_data *metadata)
{
	/* The record page is just an array of chunk records. */
	const struct uds_chunk_record *records =
		(const struct uds_chunk_record *) record_page;

	/*
	 * The array of records is sorted by name and stored as a binary tree
	 * in heap order, so the root of the tree is the first array element.
	 */
	unsigned int node = 0;

	while (node < geometry->records_per_page) {
		const struct uds_chunk_record *record = &records[node];
		int result = memcmp(name, &record->name, UDS_CHUNK_NAME_SIZE);

		if (result == 0) {
			if (metadata != NULL) {
				*metadata = record->data;
			}
			return true;
		}
		/*
		 * The children of node N are in the heap at indexes 2N+1 and
		 * 2N+2.
		 */
		node = ((2 * node) + ((result < 0) ? 1 : 2));
	}
	return false;
}

static int search_page(struct cached_page *page,
		       const struct volume *volume,
		       struct uds_request *request,
		       bool record_page)
{
	int result;
	enum uds_index_region location;
	int record_page_number;

	if (record_page) {
		if (search_record_page(dm_bufio_get_block_data(page->buffer),
				       &request->chunk_name,
				       volume->geometry,
				       &request->old_metadata)) {
			location = UDS_LOCATION_RECORD_PAGE_LOOKUP;
		} else {
			location = UDS_LOCATION_UNAVAILABLE;
		}
	} else {
		result = search_chapter_index_page(&page->cp_index_page,
						   volume->geometry,
						   &request->chunk_name,
						   &record_page_number);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (record_page_number == NO_CHAPTER_INDEX_ENTRY) {
			location = UDS_LOCATION_UNAVAILABLE;
		} else {
			location = UDS_LOCATION_INDEX_PAGE_LOOKUP;
			*((int *) &request->old_metadata) =
				record_page_number;
		}
	}

	request->location = location;
	request->found = false;
	return UDS_SUCCESS;
}

static void read_thread_function(void *arg)
{
	struct volume *volume = arg;
	byte *page_data;
	unsigned int queue_pos;
	struct uds_request *request_list;
	unsigned int physical_page;
	bool invalid = false;

	uds_log_debug("reader starting");
	uds_lock_mutex(&volume->read_threads_mutex);
	while (true) {
		bool record_page;
		struct cached_page *page = NULL;
		int result = UDS_SUCCESS;

		wait_to_reserve_read_queue_entry(volume,
						 &queue_pos,
						 &request_list,
						 &physical_page,
						 &invalid);
		if ((volume->reader_state & READER_STATE_EXIT) != 0) {
			break;
		}

		volume->busy_reader_threads++;

		record_page = is_record_page(volume->geometry, physical_page);

		if (!invalid) {
			/*
			 * Find a place to put the read queue page we reserved
			 * above.
			 */
			result = select_victim_in_cache(volume->page_cache,
							&page);
			if (result == UDS_SUCCESS) {
				uds_unlock_mutex(&volume->read_threads_mutex);

				page_data = dm_bufio_read(volume->client,
							  physical_page,
							  &page->buffer);
				if (IS_ERR(page_data)) {
					result = -PTR_ERR(page_data);
					uds_log_warning_strerror(result,
								 "error reading physical page %u from volume",
								 physical_page);
					cancel_page_in_cache(volume->page_cache,
							     physical_page,
							     page);
				}
				uds_lock_mutex(&volume->read_threads_mutex);
			} else {
				uds_log_warning("Error selecting cache victim for page read");
			}

			if (result == UDS_SUCCESS) {
				if (!volume->page_cache->read_queue[queue_pos]
					     .invalid) {
					if (!record_page) {
						result = initialize_index_page(volume,
									       physical_page,
									       page);
						if (result != UDS_SUCCESS) {
							uds_log_warning("Error initializing chapter index page");
							cancel_page_in_cache(volume->page_cache,
									     physical_page,
									     page);
						}
					}

					if (result == UDS_SUCCESS) {
						result = put_page_in_cache(volume->page_cache,
									   physical_page,
									   page);
						if (result != UDS_SUCCESS) {
							uds_log_warning("Error putting page %u in cache",
									physical_page);
							cancel_page_in_cache(volume->page_cache,
									     physical_page,
									     page);
						}
					}
				} else {
					uds_log_warning("Page %u invalidated after read",
							physical_page);
					cancel_page_in_cache(volume->page_cache,
							     physical_page,
							     page);
					invalid = true;
				}
			}
		} else {
			uds_log_debug("Requeuing requests for invalid page");
		}

		if (invalid) {
			result = UDS_SUCCESS;
			page = NULL;
		}

		while (request_list != NULL) {
			struct uds_request *request = request_list;

			request_list = request->next_request;

			/*
			 * If we've read in a record page, we're going to do an
			 * immediate search, to speed up processing by avoiding
			 * get_record_from_zone, and to ensure that requests
			 * make progress even when queued. If we've read in an
			 * index page, we save the record page number so we
			 * don't have to resolve the index page again. We use
			 * the location, virtual_chapter, and old_metadata
			 * fields in the request to allow the index code to
			 * know where to begin processing the request again.
			 */
			if ((result == UDS_SUCCESS) && (page != NULL)) {
				result = search_page(page,
						     volume,
						     request,
						     record_page);
			}

			/* reflect any read failures in the request status */
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

		release_read_queue_entry(volume->page_cache, queue_pos);

		volume->busy_reader_threads--;
		uds_broadcast_cond(&volume->read_threads_read_done_cond);
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
	bool sync_read = ((request == NULL) || (request->session == NULL));
	byte *page_data;

	if (sync_read) {
		/* Find a place to put the page. */
		result = select_victim_in_cache(volume->page_cache, &page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error selecting cache victim for page read");
			return result;
		}
		page_data = dm_bufio_read(volume->client,
					  physical_page,
					  &page->buffer);
		if (IS_ERR(page_data)) {
			result = -PTR_ERR(page_data);
			uds_log_warning_strerror(result,
						 "error reading physical page %u from volume",
						 physical_page);
			cancel_page_in_cache(volume->page_cache, physical_page,
					     page);
			return result;
		}
		if (!is_record_page(volume->geometry, physical_page)) {
			result = initialize_index_page(volume, physical_page,
						       page);
			if (result != UDS_SUCCESS) {
				if (volume->lookup_mode !=
				    LOOKUP_FOR_REBUILD) {
					uds_log_warning("Corrupt index page %u",
							physical_page);
				}
				cancel_page_in_cache(volume->page_cache,
						     physical_page,
						     page);
				return result;
			}
		}
		result = put_page_in_cache(volume->page_cache, physical_page,
					   page);
		if (result != UDS_SUCCESS) {
			uds_log_warning("Error putting page %u in cache",
					physical_page);
			cancel_page_in_cache(volume->page_cache, physical_page,
					     page);
			return result;
		}
	} else {
		result = enqueue_page_read(volume, request, physical_page);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	*page_ptr = page;

	return UDS_SUCCESS;
}

int get_volume_page_locked(struct volume *volume,
			   unsigned int physical_page,
			   struct cached_page **page_ptr)
{
	struct cached_page *page = NULL;
	int result = get_page_from_cache(volume->page_cache,
					 physical_page,
					 &page);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (page == NULL) {
		result = read_page_locked(volume, NULL, physical_page, &page);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} else {
		make_page_most_recent(volume->page_cache, page);
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

int get_volume_page_protected(struct volume *volume,
			      struct uds_request *request,
			      unsigned int physical_page,
			      struct cached_page **page_ptr)
{
	unsigned int zone_number;
	struct cached_page *page = NULL;
	int result =
		get_page_from_cache(volume->page_cache,
				    physical_page,
				    &page);
	if (result != UDS_SUCCESS) {
		return result;
	}

	zone_number = get_zone_number(request);
	/*
	 * If we didn't find a page we need to enqueue a read for it, in which
	 * case we need to grab the mutex.
	 */
	if (page == NULL) {
		end_pending_search(volume->page_cache, zone_number);
		uds_lock_mutex(&volume->read_threads_mutex);

		/*
		 * Do the lookup again while holding the read mutex (no longer
		 * the fast case so this should be ok to repeat). We need to do
		 * this because an page may have been added to the page map by
		 * the reader thread between the time searched above and the
		 * time we went to actually try to enqueue it below. This could
		 * result in us enqueuing another read for an page which is
		 * already in the cache, which would mean we end up with two
		 * entries in the cache for the same page.
		 */
		result = get_page_from_cache(volume->page_cache,
					     physical_page,
					     &page);
		if (result != UDS_SUCCESS) {
			/*
			 * In non-success cases (anything not UDS_SUCCESS,
			 * meaning both UDS_QUEUED and "real" errors), the
			 * caller doesn't get a handle on a cache page, so it
			 * can't continue the search, and we don't need to
			 * prevent other threads from messing with the cache.
			 *
			 * However, we do need to set the "search pending" flag
			 * because the callers expect it to always be set on
			 * return, even if they can't actually do the search.
			 *
			 * Doing the calls in this order ought to be faster,
			 * since we let other threads have the reader thread
			 * mutex (which can require a syscall) ASAP, and set
			 * the "search pending" state that can block the reader
			 * thread as the last thing.
			 */
			uds_unlock_mutex(&volume->read_threads_mutex);
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			return result;
		}

		/*
		 * If we found the page now, we can release the mutex and
		 * proceed as if this were the fast case.
		 */
		if (page != NULL) {
			/*
			 * If we found a page (*page_ptr != NULL and return
			 * UDS_SUCCESS), then we're telling the caller where to
			 * look for the cache page, and need to switch to
			 * "reader thread unlocked" and "search pending" state
			 * in careful order so no other thread can mess with
			 * the data before our caller gets to look at it.
			 */
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			uds_unlock_mutex(&volume->read_threads_mutex);
		}
	}

	if (page == NULL) {
		result = read_page_locked(volume, request, physical_page,
					  &page);
		if (result != UDS_SUCCESS) {
			/*
			 * This code path is used frequently in the UDS_QUEUED
			 * case, so the performance gain from unlocking first,
			 * while "search pending" mode is off, turns out to be
			 * significant in some cases.
			 */
			uds_unlock_mutex(&volume->read_threads_mutex);
			begin_pending_search(volume->page_cache,
					     physical_page,
					     zone_number);
			return result;
		}

		/* See above re: ordering requirement. */
		begin_pending_search(volume->page_cache, physical_page,
				     zone_number);
		uds_unlock_mutex(&volume->read_threads_mutex);
	} else {
		if (get_zone_number(request) == 0) {
			/* Only 1 zone is responsible for updating LRU */
			make_page_most_recent(volume->page_cache, page);
		}
	}

	*page_ptr = page;
	return UDS_SUCCESS;
}

int get_volume_page(struct volume *volume,
		    unsigned int chapter,
		    unsigned int page_number,
		    byte **data_ptr,
		    struct delta_index_page **index_page_ptr)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int physical_page =
		map_to_physical_page(volume->geometry, chapter, page_number);

	uds_lock_mutex(&volume->read_threads_mutex);
	result = get_volume_page_locked(volume, physical_page, &page);
	uds_unlock_mutex(&volume->read_threads_mutex);

	if (data_ptr != NULL) {
		*data_ptr = ((page != NULL) ?
			     dm_bufio_get_block_data(page->buffer) :
			     NULL);
	}
	if (index_page_ptr != NULL) {
		*index_page_ptr = (page != NULL) ? &page->cp_index_page : NULL;
	}
	return result;
}

/**
 * Search for a chunk name in a cached index page or chapter index, returning
 * the record page number from a chapter index match.
 *
 * @param volume              the volume containing the index page to search
 * @param request             the request originating the search (may be NULL
 *                            for a direct query from volume replay)
 * @param name                the name of the block or chunk
 * @param chapter             the chapter to search
 * @param index_page_number   the index page number of the page to search
 * @param record_page_number  pointer to return the chapter record page number
 *                            (value will be NO_CHAPTER_INDEX_ENTRY if the name
 *                            was not found)
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_cached_index_page(struct volume *volume,
				    struct uds_request *request,
				    const struct uds_chunk_name *name,
				    unsigned int chapter,
				    unsigned int index_page_number,
				    int *record_page_number)
{
	int result;
	struct cached_page *page = NULL;
	unsigned int zone_number = get_zone_number(request);
	unsigned int physical_page = map_to_physical_page(volume->geometry,
							  chapter,
							  index_page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read
	 * from the page map.  This prevents this thread from reading a page in
	 * the page map which has already been marked for invalidation by the
	 * reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume,
					   request,
					   physical_page,
					   &page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	result = ASSERT(search_pending(get_invalidate_counter(volume->page_cache, zone_number)),
			"Search is pending for zone %u",
			zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = search_chapter_index_page(&page->cp_index_page,
					   volume->geometry,
					   name,
					   record_page_number);
	end_pending_search(volume->page_cache, zone_number);
	return result;
}

int search_cached_record_page(struct volume *volume,
			      struct uds_request *request,
			      const struct uds_chunk_name *name,
			      unsigned int chapter,
			      int record_page_number,
			      struct uds_chunk_data *duplicate,
			      bool *found)
{
	struct cached_page *record_page;
	struct geometry *geometry = volume->geometry;
	int physical_page, result;
	unsigned int page_number, zone_number;

	*found = false;

	if (record_page_number == NO_CHAPTER_INDEX_ENTRY) {
		/* No record for that name can exist in the chapter. */
		return UDS_SUCCESS;
	}

	result = ASSERT(((record_page_number >= 0) &&
			     ((unsigned int) record_page_number <
			      geometry->record_pages_per_chapter)),
			    "0 <= %d <= %u",
			    record_page_number,
			    geometry->record_pages_per_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	page_number = geometry->index_pages_per_chapter + record_page_number;

	zone_number = get_zone_number(request);
	physical_page =
		map_to_physical_page(volume->geometry, chapter, page_number);

	/*
	 * Make sure the invalidate counter is updated before we try and read
	 * from the page map. This prevents this thread from reading a page in
	 * the page map which has already been marked for invalidation by the
	 * reader thread, before the reader thread has noticed that the
	 * invalidate_counter has been incremented.
	 */
	begin_pending_search(volume->page_cache, physical_page, zone_number);

	result = get_volume_page_protected(volume,
					   request,
					   physical_page,
					   &record_page);
	if (result != UDS_SUCCESS) {
		end_pending_search(volume->page_cache, zone_number);
		return result;
	}

	if (search_record_page(dm_bufio_get_block_data(record_page->buffer),
			       name,
			       geometry,
			       duplicate)) {
		*found = true;
	}
	end_pending_search(volume->page_cache, zone_number);
	return UDS_SUCCESS;
}

int read_chapter_index_from_volume(const struct volume *volume,
				   uint64_t virtual_chapter,
				   struct dm_buffer *volume_buffers[],
				   struct delta_index_page index_pages[])
{
	int result;
	unsigned int i;
	const struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter =
		map_to_physical_chapter(geometry, virtual_chapter);
	int physical_page =
		map_to_physical_page(geometry, physical_chapter, 0);

	dm_bufio_prefetch(volume->client,
			  physical_page,
			  geometry->index_pages_per_chapter);
	for (i = 0; i < geometry->index_pages_per_chapter; i++) {
		byte *index_page;
		index_page = dm_bufio_read(volume->client,
					   physical_page + i,
					   &volume_buffers[i]);
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
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

int search_volume_page_cache(struct volume *volume,
			     struct uds_request *request,
			     const struct uds_chunk_name *name,
			     uint64_t virtual_chapter,
			     struct uds_chunk_data *metadata,
			     bool *found)
{
	int result;
	unsigned int physical_chapter =
		map_to_physical_chapter(volume->geometry, virtual_chapter);
	unsigned int index_page_number;
	int record_page_number;

	index_page_number = find_index_page_number(volume->index_page_map,
						   name,
						   physical_chapter);

	if ((request != NULL) &&
	    (request->location == UDS_LOCATION_INDEX_PAGE_LOOKUP)) {
		record_page_number = *((int *) &request->old_metadata);
	} else {
		result = search_cached_index_page(volume,
						  request,
						  name,
						  physical_chapter,
						  index_page_number,
						  &record_page_number);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return search_cached_record_page(volume,
					 request,
					 name,
					 physical_chapter,
					 record_page_number,
					 metadata,
					 found);
}

int forget_chapter(struct volume *volume, uint64_t virtual_chapter)
{
	int result;
	unsigned int physical_chapter =
		map_to_physical_chapter(volume->geometry, virtual_chapter);
	uds_log_debug("forgetting chapter %llu",
		      (unsigned long long) virtual_chapter);
	uds_lock_mutex(&volume->read_threads_mutex);
	result = invalidate_page_cache_for_chapter(volume->page_cache,
						   physical_chapter,
						   volume->geometry->pages_per_chapter);
	uds_unlock_mutex(&volume->read_threads_mutex);
	return result;
}

/**
 * Donate index page data to the page cache for an index page that was just
 * written to the volume.  The caller must already hold the reader thread
 * mutex.
 *
 * @param volume             the volume
 * @param physical_chapter   the physical chapter number of the index page
 * @param index_page_number  the chapter page number of the index page
 * @param page_buffer        the index page buffer
 **/
static int donate_index_page_locked(struct volume *volume,
				    unsigned int physical_chapter,
				    unsigned int index_page_number,
				    struct dm_buffer *page_buffer)
{
	unsigned int physical_page = map_to_physical_page(volume->geometry,
							  physical_chapter,
							  index_page_number);

	/* Find a place to put the page. */
	struct cached_page *page = NULL;
	int result = select_victim_in_cache(volume->page_cache, &page);

	if (result != UDS_SUCCESS) {
		dm_bufio_release(page_buffer);
		return result;
	}

	page->buffer = page_buffer;
	result = init_chapter_index_page(volume,
					 dm_bufio_get_block_data(page_buffer),
					 physical_chapter,
					 index_page_number,
					 &page->cp_index_page);
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
		      byte **pages)
{
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry,
					chapter_index->virtual_chapter_number);
	unsigned int delta_list_number = 0;
	unsigned int index_page_number;

	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		byte *page_data;
		unsigned int lists_packed;
		bool last_page;
                int result;

		page_data = dm_bufio_new(volume->client,
					 physical_page + index_page_number,
					 &page_buffer);
		if (IS_ERR(page_data)) {
			return uds_log_warning_strerror(-PTR_ERR(page_data),
							"failed to prepare index page");
		}

		/* Pack as many delta lists into the index page as will fit. */
		last_page = ((index_page_number + 1) ==
			     geometry->index_pages_per_chapter);
		result = pack_open_chapter_index_page(chapter_index,
						      page_data,
						      delta_list_number,
						      last_page,
						      &lists_packed);
		if (result != UDS_SUCCESS) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(result,
							"failed to pack index page");
		}

#ifdef TEST_INTERNAL
		if (get_dory_forgetful()) {
			dm_bufio_release(page_buffer);
			return uds_log_warning_strerror(-EROFS,
							"failed to write chapter index page");
		}

#endif /* TEST_INTERNAL */
		dm_bufio_mark_buffer_dirty(page_buffer);

		if (pages != NULL) {
			memcpy(pages[index_page_number],
			       page_data,
			       geometry->bytes_per_page);
		}

		/*
		 * Tell the index page map the list number of the last delta
		 * list that was packed into the index page.
		 */
		if (lists_packed == 0) {
			uds_log_debug("no delta lists packed on chapter %u page %u",
				      physical_chapter_number,
				      index_page_number);
		} else {
			delta_list_number += lists_packed;
		}

		update_index_page_map(volume->index_page_map,
				      chapter_index->virtual_chapter_number,
				      physical_chapter_number,
				      index_page_number,
				      delta_list_number - 1);

		/* Donate the page data for the index page to the page cache. */
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

static unsigned int
encode_tree(byte record_page[],
	    const struct uds_chunk_record *sorted_pointers[],
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
		 * In-order traversal: copy the contents of the next record
		 * into the page at the node offset.
		 */
		memcpy(&record_page[node * BYTES_PER_RECORD],
		       sorted_pointers[next_record],
		       BYTES_PER_RECORD);
		++next_record;

		next_record = encode_tree(record_page,
					  sorted_pointers,
					  next_record,
					  child + 1,
					  node_count);
	}
	return next_record;
}

EXTERNAL_STATIC int encode_record_page(const struct volume *volume,
				       const struct uds_chunk_record records[],
				       byte record_page[])
{
	int result;
	unsigned int records_per_page = volume->geometry->records_per_page;
	const struct uds_chunk_record **record_pointers =
		volume->record_pointers;

	/*
	 * Build an array of record pointers. We'll sort the pointers by the
	 * block names in the records, which is less work than sorting the
	 * record values.
	 */
	unsigned int i;

	for (i = 0; i < records_per_page; i++) {
		record_pointers[i] = &records[i];
	}

	STATIC_ASSERT(offsetof(struct uds_chunk_record, name) == 0);
	result = radix_sort(volume->radix_sorter,
				(const byte **) record_pointers,
				records_per_page,
				UDS_CHUNK_NAME_SIZE);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Use the sorted pointers to copy the records from the chapter to the
	 * record page in tree order.
	 */
	encode_tree(record_page, record_pointers, 0, 0, records_per_page);
	return UDS_SUCCESS;
}

int write_record_pages(struct volume *volume,
		       int physical_page,
		       const struct uds_chunk_record *records,
		       byte **pages)
{
	unsigned int record_page_number;
	struct geometry *geometry = volume->geometry;
	struct dm_buffer *page_buffer;
	const struct uds_chunk_record *next_record = records;
	/* Skip over the index pages, which come before the record pages */
	physical_page += geometry->index_pages_per_chapter;

	for (record_page_number = 0;
	     record_page_number < geometry->record_pages_per_chapter;
	     record_page_number++) {
		byte *page_data;
                int result;

		page_data = dm_bufio_new(volume->client,
					 physical_page + record_page_number,
					 &page_buffer);
		if (IS_ERR(page_data)) {
			return uds_log_warning_strerror(-PTR_ERR(page_data),
							"failed to prepare record page");
		}

		/*
		 * Sort the next page of records and copy them to the record
		 * page as a binary tree stored in heap order.
		 */
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
		if (pages != NULL) {
			memcpy(pages[record_page_number],
			       page_data,
			       geometry->bytes_per_page);
		}

		dm_bufio_release(page_buffer);
	}
	return UDS_SUCCESS;
}

int write_chapter(struct volume *volume,
		  struct open_chapter_index *chapter_index,
		  const struct uds_chunk_record *records)
{
	/* Determine the position of the virtual chapter in the volume file. */
	struct geometry *geometry = volume->geometry;
	unsigned int physical_chapter_number =
		map_to_physical_chapter(geometry,
					chapter_index->virtual_chapter_number);
	int physical_page =
		map_to_physical_page(geometry, physical_chapter_number, 0);

	/* Pack and write the delta chapter index pages to the volume. */
	int result =
		write_index_pages(volume, physical_page, chapter_index, NULL);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/* Sort and write the record pages to the volume. */
	result = write_record_pages(volume, physical_page, records, NULL);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/* Flush the data to permanent storage. */
	result = -dm_bufio_write_dirty_buffers(volume->client);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result,
				       "cannot sync chapter to volume");
	}

	return result;
}

size_t get_page_cache_size(struct page_cache *cache)
{
	if (cache == NULL) {
		return 0;
	}
	return sizeof(struct delta_index_page) * cache->num_cache_entries;
}

size_t get_cache_size(struct volume *volume)
{
	size_t size = get_page_cache_size(volume->page_cache);

	if (is_sparse_geometry(volume->geometry)) {
		size += get_sparse_cache_memory_size(volume->sparse_cache);
	}
	return size;
}

static int probe_chapter(struct volume *volume,
			 unsigned int chapter_number,
			 uint64_t *virtual_chapter_number)
{
	const struct geometry *geometry = volume->geometry;
	unsigned int expected_list_number = 0;
	unsigned int i;
	uint64_t vcn, last_vcn = UINT64_MAX;

	dm_bufio_prefetch(volume->client,
			  map_to_physical_page(geometry, chapter_number, 0),
			  geometry->index_pages_per_chapter);

	for (i = 0; i < geometry->index_pages_per_chapter; ++i) {
		struct delta_index_page *page;
		int result = get_volume_page(volume,
					     chapter_number,
					     i,
					     NULL,
					     &page);
		if (result != UDS_SUCCESS) {
			return result;
		}

		vcn = page->virtual_chapter_number;
		if (last_vcn == UINT64_MAX) {
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
				      chapter_number, i, expected_list_number,
				      page->lowest_list_number);
			return UDS_CORRUPT_DATA;
		}
		expected_list_number = page->highest_list_number + 1;

		result = validate_chapter_index_page(page, geometry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (last_vcn == UINT64_MAX) {
		uds_log_error("no chapter %u virtual chapter number determined",
			      chapter_number);
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

static int probe_wrapper(void *aux,
			 unsigned int chapter_number,
			 uint64_t *virtual_chapter_number)
{
	struct volume *volume = aux;
	int result =
		probe_chapter(volume, chapter_number, virtual_chapter_number);
	if (result == UDS_CORRUPT_DATA) {
		*virtual_chapter_number = UINT64_MAX;
		return UDS_SUCCESS;
	}
	return result;
}

static int find_real_end_of_volume(struct volume *volume,
				   unsigned int limit,
				   unsigned int *limit_ptr)
{
	/*
	 * Start checking from the end of the volume. As long as we hit corrupt
	 * data, start skipping larger and larger amounts until we find real
	 * data. If we find real data, reduce the span and try again until we
	 * find the exact boundary.
	 */
	unsigned int span = 1;
	unsigned int tries = 0;

	while (limit > 0) {
		unsigned int chapter = (span > limit) ? 0 : limit - span;
		uint64_t vcn = 0;
		int result = probe_chapter(volume, chapter, &vcn);

		if (result == UDS_SUCCESS) {
			if (span == 1) {
				break;
			}
			span /= 2;
			tries = 0;
		} else if (result == UDS_CORRUPT_DATA) {
			limit = chapter;
			if (++tries > 1) {
				span *= 2;
			}
		} else {
			return uds_log_error_strerror(result,
						      "cannot determine end of volume");
		}
	}

	if (limit_ptr != NULL) {
		*limit_ptr = limit;
	}
	return UDS_SUCCESS;
}

int find_volume_chapter_boundaries(struct volume *volume,
				   uint64_t *lowest_vcn,
				   uint64_t *highest_vcn,
				   bool *is_empty)
{
	unsigned int chapter_limit = volume->geometry->chapters_per_volume;

	int result =
		find_real_end_of_volume(volume, chapter_limit, &chapter_limit);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot find end of volume");
	}

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
					uint64_t *lowest_vcn,
					uint64_t *highest_vcn,
					int (*probe_func)(void *aux,
							  unsigned int chapter,
							  uint64_t *vcn),
					struct geometry *geometry,
					void *aux)
{
	uint64_t zero_vcn;
	uint64_t lowest = UINT64_MAX;
	uint64_t highest = UINT64_MAX;
	uint64_t moved_chapter = UINT64_MAX;
	unsigned int left_chapter, right_chapter, bad_chapters = 0;
	int result;

	if (chapter_limit == 0) {
		*lowest_vcn = 0;
		*highest_vcn = 0;
		return UDS_SUCCESS;
	}

	/*
	 * This method assumes there is at most one run of contiguous bad
	 * chapters caused by unflushed writes. Either the bad spot is at the
	 * beginning and end, or somewhere in the middle. Wherever it is, the
	 * highest and lowest VCNs are adjacent to it. Otherwise the volume is
	 * cleanly saved and somewhere in the middle of it the highest VCN
	 * immediately preceeds the lowest one.
	 */

	/* It doesn't matter if this results in a bad spot (UINT64_MAX). */
	result = (*probe_func)(aux, 0, &zero_vcn);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Binary search for end of the discontinuity in the monotonically
	 * increasing virtual chapter numbers; bad spots are treated as a span
	 * of UINT64_MAX values. In effect we're searching for the index of the
	 * smallest value less than zero_vcn. In the case we go off the end it
	 * means that chapter 0 has the lowest vcn.
	 *
	 * If a virtual chapter is out-of-order, it will be the one moved by
	 * conversion. Always skip over the moved chapter when searching,
	 * adding it to the range at the end if necessary.
	 */
	if (geometry->remapped_physical > 0) {
		uint64_t remapped_vcn;

		result = (*probe_func)(aux,
				       geometry->remapped_physical,
				       &remapped_vcn);
		if (result != UDS_SUCCESS) {
			return UDS_SUCCESS;
		}

		if (remapped_vcn == geometry->remapped_virtual) {
			moved_chapter = geometry->remapped_physical;
		}
	}

	left_chapter = 0;
	right_chapter = chapter_limit;

	while (left_chapter < right_chapter) {
		uint64_t probe_vcn;
		unsigned int chapter = (left_chapter + right_chapter) / 2;

		if (chapter == moved_chapter) {
			chapter--;
		}
		result = (*probe_func)(aux, chapter, &probe_vcn);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (zero_vcn <= probe_vcn) {
			left_chapter = chapter + 1;
			if (left_chapter == moved_chapter) {
				left_chapter++;
			}
		} else {
			right_chapter = chapter;
		}
	}

	result = ASSERT(left_chapter == right_chapter,
			"left_chapter == right_chapter");
	if (result != UDS_SUCCESS) {
		return result;
	}

	left_chapter %= chapter_limit; /* in case we're at the end */

	/*
	 * At this point, left_chapter is the chapter with the lowest virtual
	 * chapter number.
	 */

	result = (*probe_func)(aux, left_chapter, &lowest);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* The moved chapter might be the lowest in the range. */
	if ((moved_chapter != UINT64_MAX) &&
	    (lowest == geometry->remapped_virtual + 1)) {
		lowest = geometry->remapped_virtual;
	}

	result = ASSERT((lowest != UINT64_MAX), "invalid lowest chapter");
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * We now circularly scan backwards, moving over any bad chapters until
	 * we find the chapter with the highest vcn (the first good chapter we
	 * encounter).
	 */

	while (highest == UINT64_MAX) {
		right_chapter =
			(right_chapter + chapter_limit - 1) % chapter_limit;
		if (right_chapter == moved_chapter) {
			continue;
		}
		result = (*probe_func)(aux, right_chapter, &highest);
		if (result != UDS_SUCCESS) {
			return result;
		}
		if (bad_chapters++ >= max_bad_chapters) {
			uds_log_error("too many bad chapters in volume: %u",
				      bad_chapters);
			return UDS_CORRUPT_DATA;
		}
	}

	*lowest_vcn = lowest;
	*highest_vcn = highest;
	return UDS_SUCCESS;
}

/**
 * Allocate a volume.
 *
 * @param config      The configuration to use
 * @param layout      The index layout
 * @param new_volume  A pointer to hold the new volume
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check allocate_volume(const struct configuration *config,
					struct index_layout *layout,
					struct volume **new_volume)
{
	struct volume *volume;
	struct geometry *geometry;
	unsigned int reserved_buffers;
	int result = UDS_ALLOCATE(1, struct volume, "volume", &volume);

	if (result != UDS_SUCCESS) {
		return result;
	}
	volume->nonce = get_uds_volume_nonce(layout);

	result = copy_geometry(config->geometry, &volume->geometry);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return uds_log_warning_strerror(result,
						"failed to allocate geometry: error");
	}
	geometry = volume->geometry;

	/* Need a buffer for each entry in the page cache */
	reserved_buffers =
		config->cache_chapters * geometry->record_pages_per_chapter;
	/* And a buffer for the chapter writer */
	reserved_buffers += 1;
	/* And a buffer for each entry in the sparse cache */
	if (is_sparse_geometry(geometry)) {
		reserved_buffers += (config->cache_chapters *
				     geometry->index_pages_per_chapter);
	}
	volume->reserved_buffers = reserved_buffers;
	result = open_uds_volume_bufio(layout,
				       geometry->bytes_per_page,
				       volume->reserved_buffers,
				       &volume->client);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = make_radix_sorter(geometry->records_per_page,
				   &volume->radix_sorter);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	result = UDS_ALLOCATE(geometry->records_per_page,
			      const struct uds_chunk_record *,
			      "record pointers",
			      &volume->record_pointers);
	if (result != UDS_SUCCESS) {
		free_volume(volume);
		return result;
	}

	if (is_sparse_geometry(geometry)) {
		result = make_sparse_cache(geometry,
					   config->cache_chapters,
					   config->zone_count,
					   &volume->sparse_cache);
		if (result != UDS_SUCCESS) {
			free_volume(volume);
			return result;
		}
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

	*new_volume = volume;
	return UDS_SUCCESS;
}

int __must_check replace_volume_storage(struct volume *volume,
					struct index_layout *layout,
					const char *name)
{
	int result;

	result = replace_index_layout_storage(layout, name);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Release all outstanding dm_bufio objects */
	invalidate_page_cache(volume->page_cache);
	invalidate_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL) {
		dm_bufio_client_destroy(UDS_FORGET(volume->client));
	}

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
	if (result != UDS_SUCCESS) {
		return result;
	}
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

	/*
	 * Start the reader threads.  If this allocation succeeds, free_volume
	 * knows that it needs to try and stop those threads.
	 */
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
		/* We only stop as many threads as actually got started. */
		volume->num_read_threads = i + 1;
	}

	*new_volume = volume;
	return UDS_SUCCESS;
}

void free_volume(struct volume *volume)
{
	if (volume == NULL) {
		return;
	}

	/*
	 * If reader_threads is NULL, then we haven't set up the reader
	 * threads.
	 */
	if (volume->reader_threads != NULL) {
		unsigned int i;
		/*
		 * Stop the reader threads.  It is ok if there aren't any of
		 * them.
		 */
		uds_lock_mutex(&volume->read_threads_mutex);
		volume->reader_state |= READER_STATE_EXIT;
		uds_broadcast_cond(&volume->read_threads_cond);
		uds_unlock_mutex(&volume->read_threads_mutex);
		for (i = 0; i < volume->num_read_threads; i++) {
			uds_join_threads(volume->reader_threads[i]);
		}
		UDS_FREE(volume->reader_threads);
		volume->reader_threads = NULL;
	}

	/* Must destroy the client AFTER freeing the caches. */
	free_page_cache(volume->page_cache);
	free_sparse_cache(volume->sparse_cache);
	if (volume->client != NULL) {
		dm_bufio_client_destroy(UDS_FORGET(volume->client));
	}

	uds_destroy_cond(&volume->read_threads_cond);
	uds_destroy_cond(&volume->read_threads_read_done_cond);
	uds_destroy_mutex(&volume->read_threads_mutex);
	free_index_page_map(volume->index_page_map);
	free_radix_sorter(volume->radix_sorter);
	UDS_FREE(volume->geometry);
	UDS_FREE(volume->record_pointers);
	UDS_FREE(volume);
}
