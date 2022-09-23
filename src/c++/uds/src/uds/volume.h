/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUME_H
#define VOLUME_H

#include <linux/atomic.h>
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

enum reader_state {
	READER_STATE_RUN = 1,
	READER_STATE_EXIT = 2,
	READER_STATE_STOP = 4
};

enum index_lookup_mode {
	/* Always do lookups in all chapters normally.  */
	LOOKUP_NORMAL,
	/* Only do a subset of lookups needed when rebuilding an index. */
	LOOKUP_FOR_REBUILD,
};

enum {
	VOLUME_CACHE_MAX_ENTRIES = (UINT16_MAX >> 1),
	VOLUME_CACHE_QUEUED_FLAG = (1 << 15),
	VOLUME_CACHE_MAX_QUEUED_READS = 4096,
};

struct request_list {
	struct uds_request *first;
	struct uds_request *last;
};

struct queued_read {
	bool invalid;
	bool reserved;
	unsigned int physical_page;
	struct request_list request_list;
};

/*
 * Value stored atomically in a search_pending_counter. The low order
 * 32 bits is the physical page number of the cached page being read.
 * The high order 32 bits is a sequence number.
 *
 * An invalidate counter is only written by its zone thread by calling
 * the begin_pending_search or end_pending_search methods.
 *
 * Any other thread that is accessing an invalidate counter is reading
 * the value in the wait_for_pending_searches method.
 */
typedef int64_t invalidate_counter_t;
/*
 * Fields of invalidate_counter_t.
 * These must be 64 bit, so an enum cannot be not used.
 */
#define PAGE_FIELD ((long) UINT_MAX) /* The page number field */
#define COUNTER_LSB (PAGE_FIELD + 1L) /* The LSB of the counter field */

struct __attribute__((aligned(CACHE_LINE_BYTES))) search_pending_counter {
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
	/* The number of index entries */
	unsigned int num_index_entries;
	/* The max number of cached entries */
	uint16_t num_cache_entries;
	/*
	 * The index used to quickly access page in cache - top bit is a
	 * 'queued' flag
	 */
	uint16_t *index;
	/* The cache */
	struct cached_page *cache;
	/*
	 * A counter for each zone to keep track of when a search is occurring
	 * within that zone.
	 */
	struct search_pending_counter *search_pending_counters;
	/* Queued reads, as a circular array, with first and last indexes */
	struct queued_read *read_queue;
	/*
	 * All entries above this point are constant once the structure has
	 * been initialized.
	 */

	/**
	 * Entries are enqueued at read_queue_last.
	 * To 'reserve' entries, we get the entry pointed to by
	 * read_queue_last_read and increment last read. This is done
	 * with a lock so if another reader thread reserves a read, it
	 * will grab the next one. After every read is completed, the
	 * reader thread calls release_read_queue_entry which
	 * increments read_queue_first until it is equal to
	 * read_queue_last_read, but only if the value pointed to by
	 * read_queue_first is no longer pending. This means that if n
	 * reads are outstanding, read_queue_first may not be
	 * incremented until the last of the reads finishes.
	 *
	 *  First                    Last
	 * ||    |    |    |    |    |    ||
	 *   LR   (1)   (2)
	 *
	 * Read thread 1 increments last read (1), then read thread 2
	 * increments it (2). When each read completes, it checks to
	 * see if it can increment first, when all concurrent reads
	 * have completed, read_queue_first should equal
	 * read_queue_last_read.
	 **/
	uint16_t read_queue_first;
	uint16_t read_queue_last_read;
	uint16_t read_queue_last;
	/* Page access counter */
	atomic64_t clock;
};

struct volume {
	/* The layout of the volume */
	struct geometry *geometry;
	/* The access to the volume's backing store */
	struct dm_bufio_client *client;
	/* The nonce used to save the volume */
	uint64_t nonce;
	/* A single page's records, for sorting */
	const struct uds_chunk_record **record_pointers;
	/* For sorting record pages */
	struct radix_sorter *radix_sorter;
	/* The sparse chapter index cache */
	struct sparse_cache *sparse_cache;
	/* The page cache */
	struct page_cache *page_cache;
	/* The index page map maps delta list numbers to index page numbers */
	struct index_page_map *index_page_map;
	/* mutex to sync between read threads and index thread */
	struct mutex read_threads_mutex;
	/* cond_var to indicate when read threads should start working */
	struct cond_var read_threads_cond;
	/* cond_var to indicate when a read thread has finished a read */
	struct cond_var read_threads_read_done_cond;
	/* Threads to read data from disk */
	struct thread **reader_threads;
	/* Number of threads busy with reads */
	unsigned int busy_reader_threads;
	/* The state of the reader threads */
	enum reader_state reader_state;
	/* The lookup mode for the index */
	enum index_lookup_mode lookup_mode;
	/* Number of read threads to use (run-time parameter) */
	unsigned int num_read_threads;
	/* Number of reserved buffers for the volume store */
	unsigned int reserved_buffers;
};

#ifdef TEST_INTERNAL
typedef void (*request_restarter_t)(struct uds_request *);

/**
 * Set the function pointer which is used to restart requests.
 * This is used as a test hook by the unit tests.
 *
 * @param restarter   The function to call to restart requests.
 **/
void set_request_restarter(request_restarter_t restarter);

/**
 * Generate the on-disk encoding of a record page from the list of records
 * in the open chapter representation.
 *
 * @param volume       The volume
 * @param records      The records to be encoded
 * @param record_page  The record page
 *
 * @return UDS_SUCCESS or an error code
 **/
int encode_record_page(const struct volume *volume,
		       const struct uds_chunk_record records[],
		       byte record_page[]);

/**
 * Find the metadata for a given block name in this page.
 *
 * @param record_page  The record page
 * @param name         The block name to look for
 * @param geometry     The geometry of the volume
 * @param metadata     an array in which to place the metadata of the
 *                     record, if one was found
 *
 * @return <code>true</code> if the record was found
 **/
bool search_record_page(const byte record_page[],
			const struct uds_record_name *name,
			const struct geometry *geometry,
			struct uds_record_data *metadata);

#endif /* TEST_INTERNAL*/
/**
 * Create a volume.
 *
 * @param config      The configuration to use.
 * @param layout      The index layout
 * @param new_volume  A pointer to hold a pointer to the new volume.
 *
 * @return          UDS_SUCCESS or an error code
 **/
int __must_check make_volume(const struct configuration *config,
			     struct index_layout *layout,
			     struct volume **new_volume);

/**
 * Clean up a volume and its memory.
 *
 * @param volume  The volume to destroy.
 **/
void free_volume(struct volume *volume);

/**
 * Replace the backing storage for a volume.
 *
 * @param volume  The volume to reconfigure
 * @param layout  The index layout
 * @param path    The path to the new backing store
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check replace_volume_storage(struct volume *volume,
					struct index_layout *layout,
					const char *path);

/**
 * Allocate a cache for a volume.
 *
 * @param geometry           The geometry governing the volume
 * @param chapters_in_cache  The size (in chapters) of the page cache
 * @param zone_count         The number of zones in the index
 * @param cache_ptr          A pointer to hold the new page cache
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_page_cache(const struct geometry *geometry,
				 unsigned int chapters_in_cache,
				 unsigned int zone_count,
				 struct page_cache **cache_ptr);

/**
 * Clean up a volume's cache
 *
 * @param cache the volumecache
 **/
void free_page_cache(struct page_cache *cache);

/**
 * Remove all entries and release all cache data from a page cache.
 *
 * @param cache  The page cache
 **/
void invalidate_page_cache(struct page_cache *cache);

/**
 * Invalidates a page cache for a particular chapter
 *
 * @param cache             the page cache
 * @param chapter           the chapter
 * @param pages_per_chapter the number of pages per chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
invalidate_page_cache_for_chapter(struct page_cache *cache,
				  unsigned int chapter,
				  unsigned int pages_per_chapter);

#ifdef TEST_INTERNAL
/**
 * Find a page, invalidate it, and make its memory the least recent. This
 * method is only exposed for the use of unit tests.
 *
 * @param cache         The cache containing the page
 * @param physical_page The id of the page to invalidate
 * @param must_find     If <code>true</code>, it is an error if the page
 *                      can't be found
 *
 * @return UDS_SUCCESS or an error code
 **/
int find_invalidate_and_make_least_recent(struct page_cache *cache,
					  unsigned int physical_page,
					  bool must_find);

#endif /* TEST_INTERNAL */
/**
 * Make the page the most recent in the cache
 *
 * @param cache    the page cache
 * @param page_ptr the page to make most recent
 **/
void make_page_most_recent(struct page_cache *cache,
			   struct cached_page *page_ptr);

/**
 * Verifies that a page is in the cache. This method is only exposed for the
 * use of unit tests.
 *
 * @param cache the cache to verify
 * @param page  the page to find
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check assert_page_in_cache(struct page_cache *cache,
				      struct cached_page *page);

/**
 * Gets a page from the cache.
 *
 * @param [in] cache         the page cache
 * @param [in] physical_page the page number
 * @param [out] page         the found page
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_page_from_cache(struct page_cache *cache,
				     unsigned int physical_page,
				     struct cached_page **page);

/**
 * Enqueue a read request
 *
 * @param cache          the page cache
 * @param request        the request that depends on the read
 * @param physical_page  the physical page for the request
 *
 * @return UDS_QUEUED    if the page was queued
 *         UDS_SUCCESS   if the queue was full
 *         an error code if there was an error
 **/
int __must_check enqueue_read(struct page_cache *cache,
			      struct uds_request *request,
			      unsigned int physical_page);

/**
 * Reserves a queued read for future dequeuing, but does not remove it from
 * the queue. Must call release_read_queue_entry to complete the process
 *
 * @param cache           the page cache
 * @param queue_pos       the position in the read queue for this pending read
 * @param first_requests  list of requests for the pending read
 * @param physical_page   the physical page for the requests
 * @param invalid         whether or not this entry is invalid
 *
 * @return UDS_SUCCESS or an error code
 **/
bool reserve_read_queue_entry(struct page_cache *cache,
			      unsigned int *queue_pos,
			      struct uds_request **first_requests,
			      unsigned int *physical_page,
			      bool *invalid);

/**
 * Releases a read from the queue, allowing it to be reused by future
 * enqueues
 *
 * @param cache      the page cache
 * @param queue_pos  queue entry position
 **/
void release_read_queue_entry(struct page_cache *cache,
			      unsigned int queue_pos);

/**
 * Return the next read queue entry position after the given position.
 *
 * @param position  The read queue entry position to increment
 *
 * @return the position of the next read queue entry
 **/
static INLINE uint16_t next_read_queue_position(uint16_t position)
{
	return (position + 1) % VOLUME_CACHE_MAX_QUEUED_READS;
}

/**
 * Check for the page cache read queue being full.
 *
 * @param cache  the page cache for which to check the read queue.
 *
 * @return  true if the read queue for cache is full, false otherwise.
 **/
static INLINE bool read_queue_is_full(struct page_cache *cache)
{
	return (cache->read_queue_first ==
		next_read_queue_position(cache->read_queue_last));
}

/**
 * Selects a page in the cache to be used for a read.
 *
 * This will clear the pointer in the page map and
 * set read_pending to true on the cache page
 *
 * @param cache     the page cache
 * @param page_ptr  the page to add
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check select_victim_in_cache(struct page_cache *cache,
					struct cached_page **page_ptr);

/**
 * Completes an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and point
 * the page map for the new page to this entry
 *
 * @param cache          the page cache
 * @param physical_page  the page number
 * @param page           the page to complete processing on
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_page_in_cache(struct page_cache *cache,
				   unsigned int physical_page,
				   struct cached_page *page);

/**
 * Cancels an async page read in the cache, so that
 * the page can now be used for incoming requests.
 *
 * This will invalidate the old cache entry and clear
 * the read queued flag on the page map entry, if it
 * was set.
 *
 * @param cache          the page cache
 * @param physical_page  the page number to clear the queued read flag on
 * @param page           the page to cancel processing on
 **/
void cancel_page_in_cache(struct page_cache *cache,
			  unsigned int physical_page,
			  struct cached_page *page);

/**
 * Get the page cache size
 *
 * @param cache the page cache
 *
 * @return the size of the page cache
 **/
size_t __must_check get_page_cache_size(struct page_cache *cache);

/**
 * Read the invalidate counter for the given zone.
 *
 * @param cache        the page cache
 * @param zone_number  the zone number
 *
 * @return the invalidate counter value
 **/
static INLINE invalidate_counter_t
get_invalidate_counter(struct page_cache *cache, unsigned int zone_number)
{
	return atomic64_read(&cache->search_pending_counters[zone_number].atomic_value);
}

/**
 * Write the invalidate counter for the given zone.
 *
 * @param cache               the page cache
 * @param zone_number         the zone number
 * @param invalidate_counter  the invalidate counter value to write
 **/
static INLINE void set_invalidate_counter(struct page_cache *cache,
					  unsigned int zone_number,
					  invalidate_counter_t invalidate_counter)
{
	atomic64_set(&cache->search_pending_counters[zone_number].atomic_value,
		     invalidate_counter);
}

/**
 * Return the physical page number of the page being searched. The return
 * value is only valid if search_pending indicates that a search is in progress.
 *
 * @param counter  the invalidate counter value to check
 *
 * @return the page that the zone is searching
 **/
static INLINE unsigned int page_being_searched(invalidate_counter_t counter)
{
	return counter & PAGE_FIELD;
}

/**
 * Determines whether a given value indicates that a search is occuring.
 *
 * @param invalidate_counter  the invalidate counter value to check
 *
 * @return true if a search is pending, false otherwise
 **/
static INLINE bool search_pending(invalidate_counter_t invalidate_counter)
{
	return (invalidate_counter & COUNTER_LSB) != 0;
}

/**
 * Increment the counter for the specified zone to signal that a search has
 * begun. Also set which page is being searched. The search_pending_counters
 * are protecting read access to pages indexed by the cache. This is the
 * "lock" action.
 *
 * @param cache          the page cache
 * @param physical_page  the page that the zone is searching
 * @param zone_number    the zone number
 **/
static INLINE void begin_pending_search(struct page_cache *cache,
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
	 * wait_for_pending_searches.
	 */
	smp_mb();
}

/**
 * Increment the counter for the specified zone to signal that a search has
 * finished. We do not need to reset the page since we only should ever look
 * at the page value if the counter indicates a search is ongoing. The
 * search_pending_counters are protecting read access to pages indexed by the
 * cache. This is the "unlock" action.
 *
 * @param cache        the page cache
 * @param zone_number  the zone number
 **/
static INLINE void end_pending_search(struct page_cache *cache,
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

/**
 * Enqueue a page read.
 *
 * @param volume         the volume
 * @param request        the request to waiting on the read
 * @param physical_page  the page number to read
 *
 * @return UDS_QUEUED if successful, or an error code
 **/
int __must_check enqueue_page_read(struct volume *volume,
				   struct uds_request *request,
				   int physical_page);

/**
 * Find the lowest and highest contiguous chapters and determine their
 * virtual chapter numbers.
 *
 * @param [in]  volume       The volume to probe.
 * @param [out] lowest_vcn   Pointer for lowest virtual chapter number.
 * @param [out] highest_vcn  Pointer for highest virtual chapter number.
 * @param [out] is_empty     Pointer to a bool indicating whether or not the
 *                           volume is empty.
 *
 * @return              UDS_SUCCESS, or an error code.
 *
 * @note        This routine does something similar to a binary search to find
 *              the location in the volume file where the discontinuity of
 *              chapter numbers occurs.  In a good save, the discontinuity is
 *              a sharp cliff, but if write failures occured during saving
 *              there may be one or more chapters which are partially written.
 *
 * @note        This method takes advantage of the fact that the physical
 *              chapter number in which the index pages are found should have
 *              headers which state that the virtual chapter number are all
 *              identical and maintain the invariant that
 *              pcn == vcn % chapters_per_volume.
 **/
int __must_check find_volume_chapter_boundaries(struct volume *volume,
						uint64_t *lowest_vcn,
						uint64_t *highest_vcn,
						bool *is_empty);

/**
 * Find any matching metadata for the given name within a given physical
 * chapter.
 *
 * @param volume           The volume.
 * @param request          The request originating the search.
 * @param name             The block name of interest.
 * @param virtual_chapter  The number of the chapter to search.
 * @param metadata         The old metadata for the name.
 * @param found            A pointer which will be set to
 *                         <code>true</code> if a match was found.
 *
 * @return UDS_SUCCESS or an error
 **/
int __must_check search_volume_page_cache(struct volume *volume,
					  struct uds_request *request,
					  const struct uds_record_name *name,
					  uint64_t virtual_chapter,
					  struct uds_record_data *metadata,
					  bool *found);

/**
 * Fetch a record page from the cache or read it from the volume and search it
 * for a record name.
 *
 * If a match is found, optionally returns the metadata from the stored
 * record. If the requested record page is not cached, the page fetch may be
 * asynchronously completed on the slow lane, in which case UDS_QUEUED will be
 * returned and the request will be requeued for continued processing after
 * the page is read and added to the cache.
 *
 * @param volume              the volume containing the record page to search
 * @param request             the request originating the search (may be NULL
 *                            for a direct query from volume replay)
 * @param name                the name of the block or chunk
 * @param chapter             the chapter to search
 * @param record_page_number  the record page number of the page to search
 * @param duplicate           an array in which to place the metadata of the
 *                            duplicate, if one was found
 * @param found               a (bool *) which will be set to true if the chunk
 *                            was found
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
int __must_check search_cached_record_page(struct volume *volume,
					   struct uds_request *request,
					   const struct uds_record_name *name,
					   unsigned int chapter,
					   int record_page_number,
					   struct uds_record_data *duplicate,
					   bool *found);

/**
 * Forget the contents of a chapter. Invalidates any cached state for the
 * specified chapter.
 *
 * @param volume   the volume containing the chapter
 * @param chapter  the virtual chapter number
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check forget_chapter(struct volume *volume, uint64_t chapter);

/**
 * Write a chapter's worth of index pages to a volume
 *
 * @param volume         the volume containing the chapter
 * @param physical_page  the page number in the volume for the chapter
 * @param chapter_index  the populated delta chapter index
 * @param pages          pointer to array of page pointers. Used only in
 *                       testing to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_index_pages(struct volume *volume,
				   int physical_page,
				   struct open_chapter_index *chapter_index,
				   byte **pages);

/**
 * Write a chapter's worth of record pages to a volume
 *
 * @param volume         the volume containing the chapter
 * @param physical_page  the page number in the volume for the chapter
 * @param records        an array of chunk records in the chapter
 * @param pages          pointer to array of page pointers. Used only in
 *                       testing to return what data has been written to disk.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_record_pages(struct volume *volume,
				    int physical_page,
				    const struct uds_chunk_record *records,
				    byte **pages);

/**
 * Write the index and records from the most recently filled chapter to the
 * volume.
 *
 * @param volume                the volume containing the chapter
 * @param chapter_index         the populated delta chapter index
 * @param records               a 1-based array of chunk records in the chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check write_chapter(struct volume *volume,
			       struct open_chapter_index *chapter_index,
			       const struct uds_chunk_record records[]);

/**
 * Read all the index pages for a chapter from the volume and initialize an
 * array of ChapterIndexPages to represent them.
 *
 * @param [in]  volume           the volume containing the chapter
 * @param [in]  virtual_chapter  the virtual chapter number of the index to
 *                               read
 * @param [out] volume_buffers   an array to receive the raw index page data
 * @param [out] index_pages      an array of ChapterIndexPages to initialize
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
read_chapter_index_from_volume(const struct volume *volume,
			       uint64_t virtual_chapter,
			       struct dm_buffer *volume_buffers[],
			       struct delta_index_page index_pages[]);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, this is done immediately in the same thread and the
 * page is then returned.
 *
 * The caller of this function must be holding the volume read mutex before
 * calling this function.
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function is only exposed for the use of unit tests.
 *
 * @param volume         The volume containing the page
 * @param physical_page  The physical page number
 * @param entry_ptr      A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_page_locked(struct volume *volume,
					unsigned int physical_page,
					struct cached_page **entry_ptr);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, the read request is enqueued for later processing
 * by another thread. When that thread finally reads the page into the cache,
 * a callback function is called to inform the caller the read is complete.
 *
 * The caller of this function should not be holding the volume read lock.
 * Instead, the caller must call begin_pending_search() for the given zone
 * the request is being processed in. That state will be maintained or
 * restored when the call returns, at which point the caller should call
 * end_pending_search().
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function is only exposed for the use of unit tests.
 *
 * @param volume         The volume containing the page
 * @param request        The request originating the search
 * @param physical_page  The physical page number
 * @param entry_ptr      A pointer to hold the retrieved cached entry
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_page_protected(struct volume *volume,
					   struct uds_request *request,
					   unsigned int physical_page,
					   struct cached_page **entry_ptr);

/**
 * Retrieve a page either from the cache (if we can) or from disk. If a read
 * from disk is required, this is done immediately in the same thread and the
 * page is then returned.
 *
 * The caller of this function must not be holding the volume read lock before
 * calling this function. This method will grab that lock and release it
 * when it returns.
 *
 * As a side-effect, the retrieved page will become the most recent page in
 * the cache.
 *
 * This function should only be called by areas of the code that do not use
 * multi-threading to access the volume. These include rebuild, volume
 * explorer, and certain unit tests.
 *
 * @param volume          The volume containing the page
 * @param chapter         The number of the chapter containing the page
 * @param page_number     The number of the page
 * @param data_ptr        Pointer to hold the retrieved page, NULL if not
 *                        wanted
 * @param index_page_ptr  Pointer to hold the retrieved chapter index page, or
 *                        NULL if not wanted
 *
 * @return UDS_SUCCESS or an error code
 **/
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

/**
 * Map a chapter number and page number to a phsical volume page number.
 *
 * @param geometry the layout of the volume
 * @param chapter  the chapter number of the desired page
 * @param page     the chapter page number of the desired page
 *
 * @return the physical page number
 **/
int __must_check map_to_physical_page(const struct geometry *geometry,
				      int chapter,
				      int page);

#endif /* VOLUME_H */
