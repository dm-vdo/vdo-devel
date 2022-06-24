// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */


#include "index.h"

#include "hash-utils.h"
#include "logger.h"
#include "open-chapter.h"
#include "request-queue.h"
#include "sparse-cache.h"

static const uint64_t NO_LAST_SAVE = UINT_MAX;
#ifdef TEST_INTERNAL
atomic_t chapters_replayed;
atomic_t chapters_written;
#endif /* TEST_INTERNAL */

struct chapter_writer {
	/* The index to which we belong */
	struct uds_index *index;
	/* The thread to do the writing */
	struct thread *thread;
	/* lock protecting the following fields */
	struct mutex mutex;
	/* condition signalled on state changes */
	struct cond_var cond;
	/* Set to true to stop the thread */
	bool stop;
	/* The result from the most recent write */
	int result;
	/* The number of bytes allocated by the chapter writer */
	size_t memory_allocated;
	/* The number of zones which have submitted a chapter for writing */
	unsigned int zones_to_write;
	/* Open chapter index used by close_open_chapter() */
	struct open_chapter_index *open_chapter_index;
	/* Collated records used by close_open_chapter() */
	struct uds_chunk_record *collated_records;
	/* The chapters to write (one per zone) */
	struct open_chapter_zone *chapters[];
};

/**
 * Get the zone for a request.
 *
 * @param index The index
 * @param request The request
 *
 * @return The zone for the request
 **/
static struct index_zone *get_request_zone(struct uds_index *index,
					   struct uds_request *request)
{
	return index->zones[request->zone_number];
}

/**
 * Check whether a chapter is sparse or dense based on the current state of
 * the index zone.
 *
 * @param zone             The index zone to check against
 * @param virtual_chapter  The virtual chapter number of the chapter to check
 *
 * @return true if the chapter is in the sparse part of the volume
 **/
static bool is_zone_chapter_sparse(const struct index_zone *zone,
				   uint64_t virtual_chapter)
{
	return is_chapter_sparse(zone->index->volume->geometry,
				 zone->oldest_virtual_chapter,
				 zone->newest_virtual_chapter,
				 virtual_chapter);
}

/**
 * Triage an index request, deciding whether it requires that a sparse cache
 * barrier message precede it.
 *
 * This resolves the chunk name in the request in the volume index,
 * determining if it is a hook or not, and if a hook, what virtual chapter (if
 * any) it might be found in. If a virtual chapter is found, it checks whether
 * that chapter appears in the sparse region of the index. If all these
 * conditions are met, the (sparse) virtual chapter number is returned. In all
 * other cases it returns <code>UINT64_MAX</code>.
 *
 * @param index	   the index that will process the request
 * @param request  the index request containing the chunk name to triage
 *
 * @return the sparse chapter number for the sparse cache barrier message, or
 *	   <code>UINT64_MAX</code> if the request does not require a barrier
 **/
static uint64_t triage_index_request(struct uds_index *index,
				     struct uds_request *request)
{
	struct volume_index_triage triage;
	struct index_zone *zone;

	lookup_volume_index_name(index->volume_index, &request->chunk_name,
				 &triage);
	if (!triage.in_sampled_chapter) {
		/* Not indexed or not a hook. */
		return UINT64_MAX;
	}

	zone = get_request_zone(index, request);
	if (!is_zone_chapter_sparse(zone, triage.virtual_chapter)) {
		return UINT64_MAX;
	}

	/*
	 * XXX Optimize for a common case by remembering the chapter from the
	 * most recent barrier message and skipping this chapter if is it the
	 * same.
	 */

	/* Return the sparse chapter number to trigger the barrier messages. */
	return triage.virtual_chapter;
}

/**
 * Construct and enqueue asynchronous control messages to add the chapter
 * index for a given virtual chapter to the sparse chapter index cache.
 *
 * @param index            the index with the relevant cache and chapter
 * @param virtual_chapter  the virtual chapter number of the chapter to cache
 **/
static void enqueue_barrier_messages(struct uds_index *index,
				     uint64_t virtual_chapter)
{
	struct uds_zone_message message = {
		.type = UDS_MESSAGE_SPARSE_CACHE_BARRIER,
		.virtual_chapter = virtual_chapter,
	};
	unsigned int zone;

	for (zone = 0; zone < index->zone_count; zone++) {
		int result = launch_zone_message(message, zone, index);

		ASSERT_LOG_ONLY((result == UDS_SUCCESS),
				"barrier message allocation");
	}
}

/**
 * Simulate the creation of a sparse cache barrier message by the triage
 * queue, and the later execution of that message in an index zone.
 *
 * If the index receiving the request is multi-zone or dense, this function
 * does nothing. This simulation is an optimization for single-zone sparse
 * indexes. It also supports unit testing of indexes without queues.
 *
 * @param zone	   the index zone responsible for the index request
 * @param request  the index request about to be executed
 *
 * @return UDS_SUCCESS always
 **/
static int simulate_index_zone_barrier_message(struct index_zone *zone,
					       struct uds_request *request)
{
	uint64_t sparse_virtual_chapter;
	/* Do nothing unless this is a single-zone sparse index. */
	if ((zone->index->zone_count > 1) ||
	    !is_sparse_geometry(zone->index->volume->geometry)) {
		return UDS_SUCCESS;
	}

	/*
	 * Check if the index request is for a sampled name in a sparse
	 * chapter.
	 */
	sparse_virtual_chapter = triage_index_request(zone->index, request);
	if (sparse_virtual_chapter == UINT64_MAX) {
		/*
		 * Not indexed, not a hook, or in a chapter that is still
		 * dense, which means there should be no change to the sparse
		 * chapter index cache.
		 */
		return UDS_SUCCESS;
	}

	/*
	 * The triage queue would have generated and enqueued a barrier message
	 * preceding this request, which we simulate by directly invoking the
	 * message function.
	 */
	return update_sparse_cache(zone, sparse_virtual_chapter);
}

/**
 * This is the request processing function for the triage stage queue. Each
 * request is resolved in the volume index, determining if it is a hook or
 * not, and if a hook, what virtual chapter (if any) it might be found in. If
 * a virtual chapter is found, this enqueues a sparse chapter cache barrier in
 * every zone before enqueueing the request in its zone.
 *
 * @param request  the request to triage
 **/
static void triage_request(struct uds_request *request)
{
	struct uds_index *index = request->index;

	/*
	 * Check if the name is a hook in the index pointing at a sparse
	 * chapter.
	 */
	uint64_t sparse_virtual_chapter = triage_index_request(index, request);

	if (sparse_virtual_chapter != UINT64_MAX) {
		/* Generate and place a barrier request on every zone queue. */
		enqueue_barrier_messages(index, sparse_virtual_chapter);
	}

	enqueue_request(request, STAGE_INDEX);
}

/**
 * Wait for the chapter writer thread to finish closing the chapter previous
 * to the one specified.
 *
 * @param index                   the index
 * @param current_chapter_number  the current chapter number
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
static int finish_previous_chapter(struct uds_index *index,
       				   uint64_t current_chapter_number)
{
	int result;
	struct chapter_writer *writer = index->chapter_writer;

	uds_lock_mutex(&writer->mutex);
	while (writer->index->newest_virtual_chapter <
	       current_chapter_number) {
		uds_wait_cond(&writer->cond, &writer->mutex);
	}
	result = writer->result;
	uds_unlock_mutex(&writer->mutex);

	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "Writing of previous open chapter failed");
	}
	return UDS_SUCCESS;
}

/**
 * Swap the open and writing chapters after blocking until there are no active
 * chapter writers on the index.
 *
 * @param zone  The zone swapping chapters
 *
 * @return UDS_SUCCESS or a return code
 **/
static int swap_open_chapter(struct index_zone *zone)
{
	struct open_chapter_zone *temp_chapter;
	/* Wait for any currently writing chapter to complete */
	int result = finish_previous_chapter(zone->index,
					     zone->newest_virtual_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Swap the writing and open chapters */
	temp_chapter = zone->open_chapter;
	zone->open_chapter = zone->writing_chapter;
	zone->writing_chapter = temp_chapter;
	return UDS_SUCCESS;
}

/**
 * Advance to a new open chapter, and forget the oldest chapter in the
 * index if necessary.
 *
 * @param zone                 The zone containing the chapter to reap
 *
 * @return UDS_SUCCESS or an error code
 **/
static int reap_oldest_chapter(struct index_zone *zone)
{
	struct uds_index *index = zone->index;
	unsigned int chapters_per_volume =
		index->volume->geometry->chapters_per_volume;
	int result =
		ASSERT(((zone->newest_virtual_chapter -
			 zone->oldest_virtual_chapter) <= chapters_per_volume),
		       "newest (%llu) and oldest (%llu) virtual chapters less than or equal to chapters per volume (%u)",
		       (unsigned long long) zone->newest_virtual_chapter,
		       (unsigned long long) zone->oldest_virtual_chapter,
		       chapters_per_volume);
	if (result != UDS_SUCCESS) {
		return result;
	}

	set_volume_index_zone_open_chapter(index->volume_index, zone->id,
					   zone->newest_virtual_chapter);
	return UDS_SUCCESS;
}

/**
 * Asychronously close and write a chapter by passing it to the writer
 * thread. Writing won't start until all zones have submitted a chapter.
 *
 * @param index        the index
 * @param zone_number  the number of the zone submitting a chapter
 * @param chapter      the chapter to write
 *
 * @return The number of zones which have submitted the current chapter
 **/
static unsigned int start_closing_chapter(struct uds_index *index,
					  unsigned int zone_number,
					  struct open_chapter_zone *chapter)
{
	unsigned int finished_zones;
	struct chapter_writer *writer = index->chapter_writer;

	uds_lock_mutex(&writer->mutex);
	finished_zones = ++writer->zones_to_write;
	writer->chapters[zone_number] = chapter;
	uds_broadcast_cond(&writer->cond);
	uds_unlock_mutex(&writer->mutex);

	return finished_zones;
}

/**
 * Announce the closure of the current open chapter to the other zones.
 *
 * @param zone            The zone which first closed the chapter
 * @param closed_chapter  The chapter which was closed
 *
 * @return UDS_SUCCESS or an error code
 **/
static int announce_chapter_closed(struct index_zone *zone,
				   uint64_t closed_chapter)
{
	struct uds_zone_message zone_message = {
		.type = UDS_MESSAGE_ANNOUNCE_CHAPTER_CLOSED,
		.virtual_chapter = closed_chapter,
	};

	unsigned int i;

	for (i = 0; i < zone->index->zone_count; i++) {
		int result;

		if (zone->id == i) {
			continue;
		}
		result = launch_zone_message(zone_message, i, zone->index);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Open the next chapter.
 *
 * @param zone  The zone containing the open chapter
 *
 * @return UDS_SUCCESS if successful
 **/
static int open_next_chapter(struct index_zone *zone)
{
	uint64_t closed_chapter, victim;
	int result;
	unsigned int finished_zones;
	unsigned int expired_chapters;

	uds_log_debug("closing chapter %llu of zone %u after %u entries (%u short)",
		      (unsigned long long) zone->newest_virtual_chapter,
		      zone->id,
		      zone->open_chapter->size,
		      zone->open_chapter->capacity - zone->open_chapter->size);

	result = swap_open_chapter(zone);
	if (result != UDS_SUCCESS) {
		return result;
	}

	closed_chapter = zone->newest_virtual_chapter++;
	result = reap_oldest_chapter(zone);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "reap_oldest_chapter failed");
	}

	reset_open_chapter(zone->open_chapter);

	finished_zones = start_closing_chapter(zone->index,
					       zone->id,
					       zone->writing_chapter);
	if ((finished_zones == 1) && (zone->index->zone_count > 1)) {
		/*
		 * This is the first zone of a multi-zone index to close this
		 * chapter, so inform the other zones in order to control zone
		 * skew.
		 */
		result = announce_chapter_closed(zone, closed_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	victim = zone->oldest_virtual_chapter;
	expired_chapters = chapters_to_expire(zone->index->volume->geometry,
					      zone->newest_virtual_chapter);
	zone->oldest_virtual_chapter += expired_chapters;

	if (finished_zones < zone->index->zone_count) {
		/* We are not the last zone to close the chapter, so we're done */
		return UDS_SUCCESS;
	}

	/*
	 * We are the last zone to close the chapter, so clean up the cache.
	 * That it is safe to let the last thread out of the previous chapter
	 * to do this relies on the fact that although the new open chapter
	 * shadows the oldest chapter in the cache, until we write the new open
	 * chapter to disk, we'll never look for it in the cache.
	 */
	while ((expired_chapters-- > 0) && (result == UDS_SUCCESS)) {
		result = forget_chapter(zone->index->volume, victim++);
	}

	return result;
}

/**
 * Handle notification that some other zone has closed its open chapter. If
 * the chapter that was closed is still the open chapter for this zone,
 * close it now in order to minimize skew.
 *
 * @param zone             The zone receiving the notification
 * @param virtual_chapter  The closed virtual chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
static int handle_chapter_closed(struct index_zone *zone,
				 uint64_t virtual_chapter)
{
	if (zone->newest_virtual_chapter == virtual_chapter) {
		return open_next_chapter(zone);
	}

	return UDS_SUCCESS;
}

/**
 * Dispatch a control request to an index zone.
 *
 * @param request The request to dispatch
 *
 * @return UDS_SUCCESS or an error code
 **/
static int dispatch_index_zone_control_request(struct uds_request *request)
{
	struct uds_zone_message *message = &request->zone_message;
	struct index_zone *zone = request->index->zones[request->zone_number];

	switch (message->type) {
	case UDS_MESSAGE_SPARSE_CACHE_BARRIER:
		return update_sparse_cache(zone, message->virtual_chapter);

	case UDS_MESSAGE_ANNOUNCE_CHAPTER_CLOSED:
		return handle_chapter_closed(zone, message->virtual_chapter);

	default:
		uds_log_error("invalid message type: %d", message->type);
		return UDS_INVALID_ARGUMENT;
	}
}

/**
 * Determine the index region in which a block was found.
 *
 * @param zone                The zone that was searched
 * @param virtual_chapter     The virtual chapter number
 *
 * @return the index region of the chapter in which the block was found
 **/
static enum uds_index_region
compute_index_region(const struct index_zone *zone,
		     uint64_t virtual_chapter)
{
	if (virtual_chapter == zone->newest_virtual_chapter) {
		return UDS_LOCATION_IN_OPEN_CHAPTER;
	}
	if (is_zone_chapter_sparse(zone, virtual_chapter)) {
		return UDS_LOCATION_IN_SPARSE;
	}
	return UDS_LOCATION_IN_DENSE;
}

/**
 * Search the cached sparse chapter index, either for a cached sparse hook, or
 * as the last chance for finding the record named by a request.
 *
 * @param [in]  zone             the index zone
 * @param [in]  request          the request originating the search
 * @param [in]  virtual_chapter  if UINT64_MAX, search the entire cache;
 *                               otherwise search this chapter, if cached
 * @param [out] found            A pointer to a bool which will be set to
 *                               <code>true</code> if the record was found
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_sparse_cache_in_zone(struct index_zone *zone,
				       struct uds_request *request,
				       uint64_t virtual_chapter,
				       bool *found)
{
	struct volume *volume;
	int record_page_number;
	unsigned int chapter;
	int result = search_sparse_cache(zone,
					 &request->chunk_name,
					 &virtual_chapter,
					 &record_page_number);
	if ((result != UDS_SUCCESS) || (virtual_chapter == UINT64_MAX)) {
		return result;
	}

	request->virtual_chapter = virtual_chapter;

	volume = zone->index->volume;
	chapter = map_to_physical_chapter(volume->geometry, virtual_chapter);
	return search_cached_record_page(volume,
					 request, &request->chunk_name,
					 chapter, record_page_number,
					 &request->old_metadata, found);
}

/**
 * Get a record from either the volume or the open chapter in a zone.
 *
 * @param zone             The index zone to query
 * @param request          The request originating the query
 * @param found            A pointer to a bool which will be set to
 *                         <code>true</code> if the record was found.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_record_from_zone(struct index_zone *zone,
				struct uds_request *request,
				bool *found)
{
	struct volume *volume;

	if (request->location == UDS_LOCATION_RECORD_PAGE_LOOKUP) {
		*found = true;
		return UDS_SUCCESS;
	} else if (request->location == UDS_LOCATION_UNAVAILABLE) {
		*found = false;
		return UDS_SUCCESS;
	}

	if (request->virtual_chapter == zone->newest_virtual_chapter) {
		search_open_chapter(zone->open_chapter,
				    &request->chunk_name,
				    &request->old_metadata,
				    found);
		return UDS_SUCCESS;
	}

	if ((zone->newest_virtual_chapter > 0) &&
	    (request->virtual_chapter == (zone->newest_virtual_chapter - 1)) &&
	    (zone->writing_chapter->size > 0)) {
		/*
		 * Only search the writing chapter if it is full, else look on
		 * disk.
		 */
		search_open_chapter(zone->writing_chapter,
				    &request->chunk_name,
				    &request->old_metadata,
				    found);
		return UDS_SUCCESS;
	}

	volume = zone->index->volume;
	if (is_zone_chapter_sparse(zone, request->virtual_chapter) &&
	    sparse_cache_contains(volume->sparse_cache,
				  request->virtual_chapter,
				  request->zone_number)) {
		/*
		 * The named chunk, if it exists, is in a sparse chapter that
		 * is cached, so just run the chunk through the sparse chapter
		 * cache search.
		 */
		return search_sparse_cache_in_zone(zone,
						   request,
						   request->virtual_chapter,
						   found);
	}

	return search_volume_page_cache(volume,
					request, &request->chunk_name,
					request->virtual_chapter,
					&request->old_metadata, found);
}

/**
 * Put a record in the open chapter. If this fills the chapter, the chapter
 * will be closed and a new one will be opened.
 *
 * @param zone     The index zone containing the chapter
 * @param request  The request containing the name of the record
 * @param metadata The record metadata
 *
 * @return UDS_SUCCESS or an error
 **/
static int put_record_in_zone(struct index_zone *zone,
			      struct uds_request *request,
			      const struct uds_chunk_data *metadata)
{
	unsigned int remaining;
	int result = put_open_chapter(zone->open_chapter, &request->chunk_name,
				      metadata, &remaining);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (remaining == 0) {
		return open_next_chapter(zone);
	}

	return UDS_SUCCESS;
}

/**
 * Search an index zone. This function is only correct for LRU.
 *
 * @param zone     The index zone to query
 * @param request  The request originating the query
 *
 * @return UDS_SUCCESS or an error code
 **/
static int search_index_zone(struct index_zone *zone,
			     struct uds_request *request)
{
	int result;
	struct volume_index_record record;
	enum uds_index_region location;
	bool overflow_record, found = false;
	struct uds_chunk_data *metadata;
	uint64_t chapter;

	result = get_volume_index_record(zone->index->volume_index,
					 &request->chunk_name,
					 &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (record.is_found) {
		if (request->requeued &&
		    request->virtual_chapter != record.virtual_chapter) {
			set_request_location(request, UDS_LOCATION_UNKNOWN);
		}

		request->virtual_chapter = record.virtual_chapter;
		result = get_record_from_zone(zone, request, &found);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (found) {
		location = compute_index_region(zone, record.virtual_chapter);
		set_request_location(request, location);
	}

	/*
	 * If a record has overflowed a chapter index in more than one chapter
	 * (or overflowed in one chapter and collided with an existing record),
	 * it will exist as a collision record in the volume index, but
	 * we won't find it in the volume. This case needs special handling.
	 */
	overflow_record = (record.is_found && record.is_collision && !found);
	chapter = zone->newest_virtual_chapter;
	if (found || overflow_record) {
		if ((request->type == UDS_QUERY_NO_UPDATE) ||
		    ((request->type == UDS_QUERY) && overflow_record)) {
			/* This is a query without update, or with nothing to
			 * update */
			return UDS_SUCCESS;
		}

		if (record.virtual_chapter != chapter) {
			/*
			 * Update the volume index to reference the new chapter
			 * for the block. If the record had been deleted or
			 * dropped from the chapter index, it will be back.
			 */
			result = set_volume_index_record_chapter(&record,
								 chapter);
		} else if (request->type != UDS_UPDATE) {
			/* The record is already in the open chapter, so we're
			 * done */
			return UDS_SUCCESS;
		}
	} else {
		/*
		 * The record wasn't in the volume index, so check whether the
		 * name is in a cached sparse chapter. If we found the name on
		 * a previous search, use that result instead.
		 */
		if (request->location == UDS_LOCATION_RECORD_PAGE_LOOKUP) {
			found = true;
		} else if (request->location == UDS_LOCATION_UNAVAILABLE) {
			found = false;
		} else if (is_sparse_geometry(zone->index->volume->geometry) &&
			   !is_volume_index_sample(zone->index->volume_index,
						   &request->chunk_name)) {
			/*
			 * Passing UINT64_MAX triggers a search of the entire
			 * sparse cache.
			 */
			result = search_sparse_cache_in_zone(zone, request,
							     UINT64_MAX,
							     &found);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}

		if (found) {
			set_request_location(request, UDS_LOCATION_IN_SPARSE);
		}

		if ((request->type == UDS_QUERY_NO_UPDATE) ||
		    ((request->type == UDS_QUERY) && !found)) {
			/*
			 * This is a query without update or for a new record,
			 * so we're done.
			 */
			return UDS_SUCCESS;
		}

		/*
		 * Add a new entry to the volume index referencing the open
		 * chapter. This needs to be done both for new records, and for
		 * records from cached sparse chapters.
		 */
		result = put_volume_index_record(&record, chapter);
	}

	if (result == UDS_OVERFLOW) {
		/*
		 * The volume index encountered a delta list overflow.	The
		 * condition was already logged. We will go on without adding
		 * the chunk to the open chapter.
		 */
		return UDS_SUCCESS;
	}

	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!found || (request->type == UDS_UPDATE)) {
		/* This is a new record or we're updating an existing record. */
		metadata = &request->new_metadata;
	} else {
		/*
		 * This is a duplicate, so move the record to the open chapter
		 * (for LRU).
		 */
		metadata = &request->old_metadata;
	}
	return put_record_in_zone(zone, request, metadata);
}

static int remove_from_index_zone(struct index_zone *zone,
				  struct uds_request *request)
{
	int result;
	enum uds_index_region location;
	struct volume_index_record record;

	result = get_volume_index_record(zone->index->volume_index,
					 &request->chunk_name,
					 &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!record.is_found) {
		/*
		 * The name does not exist in volume index, so there is nothing
		 * to remove.
		 */
		return UDS_SUCCESS;
	}

	/*
	 * If the request was requeued, check whether the saved state is still
	 * valid.
	 */

	if (record.is_collision) {
		location = compute_index_region(zone, record.virtual_chapter);
		set_request_location(request, location);
	} else {
		/*
		 * Non-collision records are hints, so resolve the name in the
		 * chapter.
		 */
		bool found;

		if (request->requeued &&
		    request->virtual_chapter != record.virtual_chapter) {
			set_request_location(request, UDS_LOCATION_UNKNOWN);
		}

		request->virtual_chapter = record.virtual_chapter;
		result = get_record_from_zone(zone, request, &found);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (!found) {
			/* There is no record to remove. */
			return UDS_SUCCESS;
		}
	}

	location = compute_index_region(zone, record.virtual_chapter);
	set_request_location(request, location);

	/*
	 * Delete the volume index entry for the named record only. Note that a
	 * later search might later return stale advice if there is a colliding
	 * name in the same chapter, but it's a very rare case (1 in 2^21).
	 */
	result = remove_volume_index_record(&record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * If the record is in the open chapter, we must remove it or mark it
	 * deleted to avoid trouble if the record is added again later.
	 */
	if (request->location == UDS_LOCATION_IN_OPEN_CHAPTER) {
		bool hash_exists = false;

		remove_from_open_chapter(zone->open_chapter,
					 &request->chunk_name,
					 &hash_exists);
		result = ASSERT(hash_exists,
				"removing record not found in open chapter");
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Perform the index operation specified by the type field of a UDS request.
 *
 * For UDS API requests, this searches the index for the chunk name in the
 * request. If the chunk name is already present in the index, the location
 * field of the request will be set to the uds_index_region where it was
 * found. If the action is not DELETE, the old_metadata field of the request
 * will also be filled in with the prior metadata for the name.
 *
 * If the API request type is:
 *
 *   UDS_POST, a record will be added to the open chapter with the metadata
 *     in the request for new records, and the existing metadata for existing
 *     records.
 *
 *   UDS_UPDATE, a record will be added to the open chapter with the metadata
 *     in the request.
 *
 *   UDS_DELETE, any entry with the name will removed from the index.
 *
 *   UDS_QUERY, any record found will be moved to the open chapter.
 *
 *   UDS_QUERY_NO_UPDATE, the contents of the index will remain unchanged.
 *
 * @param index	      The index
 * @param request     The originating request
 *
 * @return UDS_SUCCESS, UDS_QUEUED, or an error code
 **/
static int dispatch_index_request(struct uds_index *index,
				  struct uds_request *request)
{
	int result;
	struct index_zone *zone = get_request_zone(index, request);

	if (!request->requeued) {
		/*
		 * Single-zone sparse indexes don't have a triage queue to
		 * generate cache barrier requests, so see if we need to
		 * synthesize a barrier.
		 */
		int result =
			simulate_index_zone_barrier_message(zone, request);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	switch (request->type) {
	case UDS_POST:
	case UDS_UPDATE:
	case UDS_QUERY:
	case UDS_QUERY_NO_UPDATE:
		result = search_index_zone(zone, request);
		break;

	case UDS_DELETE:
		result = remove_from_index_zone(zone, request);
		break;

	default:
		result = uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						  "invalid request type: %d",
						  request->type);
		break;
	}

	return result;
}

/**
 * This is the request processing function invoked by the zone's
 * uds_request_queue worker thread.
 *
 * @param request  the request to be indexed or executed by the zone worker
 **/
static void execute_zone_request(struct uds_request *request)
{
	int result;
	struct uds_index *index = request->index;

	if (request->zone_message.type != UDS_MESSAGE_NONE) {
		result = dispatch_index_zone_control_request(request);
		if (result != UDS_SUCCESS) {
			uds_log_error_strerror(result,
					       "error executing message: %d",
					       request->zone_message.type);
		}
		/*
		 * Asynchronous control messages are complete when they are
		 * executed. There should be nothing they need to do on the
		 * callback thread. The message has been completely processed,
		 * so just free it.
		 */
		UDS_FREE(UDS_FORGET(request));
		return;
	}

	index->need_to_save = true;
	if (request->requeued && (request->status != UDS_SUCCESS)) {
		set_request_location(request, UDS_LOCATION_UNAVAILABLE);
		index->callback(request);
		return;
	}

	result = dispatch_index_request(index, request);
	if (result == UDS_QUEUED) {
		/* Take the request off the pipeline. */
		return;
	}

	if (!request->found) {
		set_request_location(request, UDS_LOCATION_UNAVAILABLE);
	}
	request->status = result;
	index->callback(request);
}

/**
 * Advance the newest virtual chapter. If this will overwrite the oldest
 * virtual chapter, advance that also.
 *
 * @param index The index to advance
 **/
static void advance_active_chapters(struct uds_index *index)
{
	index->newest_virtual_chapter++;
	index->oldest_virtual_chapter +=
		chapters_to_expire(index->volume->geometry,
				   index->newest_virtual_chapter);
}

/**
 * This is the driver function for the writer thread. It loops until
 * terminated, waiting for a chapter to provided to close.
 **/
static void close_chapters(void *arg)
{
	int result;
	struct chapter_writer *writer = arg;

	uds_log_debug("chapter writer starting");
	uds_lock_mutex(&writer->mutex);
	for (;;) {
		while (writer->zones_to_write < writer->index->zone_count) {
			if (writer->stop && (writer->zones_to_write == 0)) {
				/*
				 * We've been told to stop, and all of the
				 * zones are in the same open chapter, so we
				 * can exit now.
				 */
				uds_unlock_mutex(&writer->mutex);
				uds_log_debug("chapter writer stopping");
				return;
			}
			uds_wait_cond(&writer->cond, &writer->mutex);
		}

		/*
		 * Release the lock while closing a chapter. We probably don't
		 * need to do this, but it seems safer in principle. It's OK to
		 * access the chapter and chapterNumber fields without the lock
		 * since those aren't allowed to change until we're done.
		 */
		uds_unlock_mutex(&writer->mutex);

		if (writer->index->has_saved_open_chapter) {
			/*
			 * Remove the saved open chapter as that chapter is
			 * about to be written to the volume.  This matters the
			 * first time we close the open chapter after loading
			 * from a clean shutdown, or after doing a clean save.
			 */
			writer->index->has_saved_open_chapter = false;
			result = discard_open_chapter(writer->index->layout);
			if (result == UDS_SUCCESS) {
				uds_log_debug("Discarding saved open chapter");
			}
		}

		result =
			close_open_chapter(writer->chapters,
					   writer->index->zone_count,
					   writer->index->volume,
					   writer->open_chapter_index,
					   writer->collated_records,
					   writer->index->newest_virtual_chapter);

#ifdef TEST_INTERNAL
		/*
		 * We may be synchronizing with a test waiting for a chapter to
		 * be written, so we need a memory barrier here.
		 */
		smp_mb__before_atomic();
		atomic_inc(&chapters_written);
#endif /* TEST_INTERNAL */

		uds_lock_mutex(&writer->mutex);
		/*
		 * Note that the index is totally finished with the writing
		 * chapter
		 */
		advance_active_chapters(writer->index);
		writer->result = result;
		writer->zones_to_write = 0;
		uds_broadcast_cond(&writer->cond);
	}
}

/**
 * Stop the chapter writer and wait for it to finish.
 *
 * @param writer  the chapter writer to stop
 *
 * @return UDS_SUCCESS or an error code from the most recent write
 *         request
 **/
static int stop_chapter_writer(struct chapter_writer *writer)
{
	int result;
	struct thread *writer_thread = 0;

	uds_lock_mutex(&writer->mutex);
	if (writer->thread != 0) {
		writer_thread = writer->thread;
		writer->thread = 0;
		writer->stop = true;
		uds_broadcast_cond(&writer->cond);
	}
	result = writer->result;
	uds_unlock_mutex(&writer->mutex);

	if (writer_thread != 0) {
		uds_join_threads(writer_thread);
	}

	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "Writing of previous open chapter failed");
	}
	return UDS_SUCCESS;
}

/**
 * Free a chapter writer, waiting for its thread to finish.
 *
 * @param writer  the chapter writer to destroy
 **/
static void free_chapter_writer(struct chapter_writer *writer)
{
	int result __always_unused;

	if (writer == NULL) {
		return;
	}

	result = stop_chapter_writer(writer);
	uds_destroy_mutex(&writer->mutex);
	uds_destroy_cond(&writer->cond);
	free_open_chapter_index(writer->open_chapter_index);
	UDS_FREE(writer->collated_records);
	UDS_FREE(writer);
}

/**
 * Create a chapter writer and start its thread.
 *
 * @param index       the index containing the chapters to be written
 * @param writer_ptr  pointer to hold the new writer
 *
 * @return           UDS_SUCCESS or an error code
 **/
static int make_chapter_writer(struct uds_index *index,
			       struct chapter_writer **writer_ptr)
{
	struct chapter_writer *writer;
	size_t collated_records_size =
		(sizeof(struct uds_chunk_record) *
		 (1 + index->volume->geometry->records_per_chapter));
	int result = UDS_ALLOCATE_EXTENDED(struct chapter_writer,
					   index->zone_count,
					   struct open_chapter_zone *,
					   "Chapter Writer",
					   &writer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	writer->index = index;

	result = uds_init_mutex(&writer->mutex);
	if (result != UDS_SUCCESS) {
		UDS_FREE(writer);
		return result;
	}
	result = uds_init_cond(&writer->cond);
	if (result != UDS_SUCCESS) {
		uds_destroy_mutex(&writer->mutex);
		UDS_FREE(writer);
		return result;
	}

	/*
	 * Now that we have the mutex+cond, it is safe to call
	 * free_chapter_writer.
	 */
	result = uds_allocate_cache_aligned(collated_records_size,
					    "collated records",
					    &writer->collated_records);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return result;
	}
	result = make_open_chapter_index(&writer->open_chapter_index,
					 index->volume->geometry,
					 index->volume->nonce);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return result;
	}

	writer->memory_allocated =
		(sizeof(struct chapter_writer) +
		 index->zone_count * sizeof(struct open_chapter_zone *) +
		 collated_records_size +
		 writer->open_chapter_index->memory_allocated);

	/* We're initialized, so now it's safe to start the writer thread. */
	result = uds_create_thread(close_chapters, writer, "writer",
				   &writer->thread);
	if (result != UDS_SUCCESS) {
		free_chapter_writer(writer);
		return result;
	}

	*writer_ptr = writer;
	return UDS_SUCCESS;
}

/**
 * Initialize the zone queues and the triage queue.
 *
 * @param index     the index containing the queues
 * @param geometry  the geometry governing the indexes
 *
 * @return  UDS_SUCCESS or error code
 **/
static int initialize_index_queues(struct uds_index *index,
				   const struct geometry *geometry)
{
	unsigned int i;

	for (i = 0; i < index->zone_count; i++) {
		int result = make_uds_request_queue("indexW",
						    &execute_zone_request,
						    &index->zone_queues[i]);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	/* The triage queue is only needed for sparse multi-zone indexes. */
	if ((index->zone_count > 1) && is_sparse_geometry(geometry)) {
		int result = make_uds_request_queue("triageW", &triage_request,
						    &index->triage_queue);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Set the active chapter numbers for a zone based on its index. The active
 * chapters consist of the range of chapters from the current oldest to
 * the current newest virtual chapter.
 *
 * @param zone          The zone to set
 **/
static void set_active_chapters(struct index_zone *zone)
{
	zone->oldest_virtual_chapter = zone->index->oldest_virtual_chapter;
	zone->newest_virtual_chapter = zone->index->newest_virtual_chapter;
}

static int load_index(struct uds_index *index)
{
	uint64_t last_save_chapter;
	unsigned int i;

	int result = load_index_state(index->layout, index);

	if (result != UDS_SUCCESS) {
		return UDS_INDEX_NOT_SAVED_CLEANLY;
	}

	last_save_chapter = ((index->last_save != NO_LAST_SAVE) ?
			     index->last_save : 0);

	uds_log_info("loaded index from chapter %llu through chapter %llu",
		     (unsigned long long) index->oldest_virtual_chapter,
		     (unsigned long long) last_save_chapter);

	for (i = 0; i < index->zone_count; i++) {
		set_active_chapters(index->zones[i]);
	}

	return UDS_SUCCESS;
}


static int rebuild_index_page_map(struct uds_index *index, uint64_t vcn)
{
	struct geometry *geometry = index->volume->geometry;
	unsigned int chapter = map_to_physical_chapter(geometry, vcn);
	unsigned int expected_list_number = 0;
	unsigned int index_page_number;

	for (index_page_number = 0;
	     index_page_number < geometry->index_pages_per_chapter;
	     index_page_number++) {
		unsigned int lowest_delta_list, highest_delta_list;
		struct delta_index_page *chapter_index_page;
		int result = get_volume_page(index->volume,
					     chapter,
					     index_page_number,
					     NULL,
					     &chapter_index_page);
		if (result != UDS_SUCCESS) {
			return uds_log_error_strerror(result,
						      "failed to read index page %u in chapter %u",
						      index_page_number,
						      chapter);
		}
		lowest_delta_list = chapter_index_page->lowest_list_number;
		highest_delta_list = chapter_index_page->highest_list_number;
		if (lowest_delta_list != expected_list_number) {
			return uds_log_error_strerror(UDS_CORRUPT_DATA,
						      "chapter %u index page %u is corrupt",
						      chapter,
						      index_page_number);
		}

		update_index_page_map(index->volume->index_page_map,
				      vcn,
				      chapter,
				      index_page_number,
				      highest_delta_list);
		expected_list_number = highest_delta_list + 1;
	}
	return UDS_SUCCESS;
}

/**
 * Add an entry to the volume index when rebuilding.
 *
 * @param index			  The index to query.
 * @param name			  The block name of interest.
 * @param virtual_chapter	  The virtual chapter number to write to the
 *				  volume index
 * @param will_be_sparse_chapter  True if this entry will be in the sparse
 *				  portion of the index at the end of
 *				  rebuilding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int replay_record(struct uds_index *index,
			 const struct uds_chunk_name *name,
			 uint64_t virtual_chapter,
			 bool will_be_sparse_chapter)
{
	struct volume_index_record record;
	bool update_record;
	int result;

	if (will_be_sparse_chapter &&
	    !is_volume_index_sample(index->volume_index, name)) {
		/*
		 * This entry will be in a sparse chapter after the rebuild
		 * completes, and it is not a sample, so just skip over it.
		 */
		return UDS_SUCCESS;
	}

	result = get_volume_index_record(index->volume_index, name, &record);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (record.is_found) {
		if (record.is_collision) {
			if (record.virtual_chapter == virtual_chapter) {
				/* The record is already correct, so we don't
				 * need to do anything */
				return UDS_SUCCESS;
			}
			update_record = true;
		} else if (record.virtual_chapter == virtual_chapter) {
			/*
			 * There is a volume index entry pointing to the
			 * current chapter, but we don't know if it is for the
			 * same name as the one we are currently working on or
			 * not. For now, we're just going to assume that it
			 * isn't. This will create one extra collision record
			 * if there was a deleted record in the current
			 * chapter.
			 */
			update_record = false;
		} else {
			/*
			 * If we're rebuilding, we don't normally want to go to
			 * disk to see if the record exists, since we will
			 * likely have just read the record from disk (i.e. we
			 * know it's there). The exception to this is when we
			 * already find an entry in the volume index that has a
			 * different chapter. In this case, we need to search
			 * that chapter to determine if the volume index entry
			 * was for the same record or a different one.
			 */
			result = search_volume_page_cache(index->volume,
							  NULL, name,
							  record.virtual_chapter,
							  NULL, &update_record);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	} else {
		update_record = false;
	}

	if (update_record) {
		/*
		 * Update the volume index to reference the new chapter for the
		 * block. If the record had been deleted or dropped from the
		 * chapter index, it will be back.
		 */
		result = set_volume_index_record_chapter(&record,
							 virtual_chapter);
	} else {
		/*
		 * Add a new entry to the volume index referencing the open
		 * chapter. This should be done regardless of whether we are a
		 * brand new record or a sparse record, i.e. one that doesn't
		 * exist in the index but does on disk, since for a sparse
		 * record, we would want to un-sparsify if it did exist.
		 */
		result = put_volume_index_record(&record, virtual_chapter);
	}

	if ((result == UDS_DUPLICATE_NAME) || (result == UDS_OVERFLOW)) {
		/* Ignore duplicate record and delta list overflow errors */
		return UDS_SUCCESS;
	}

	return result;
}

/**
 * Suspend the index if necessary and wait for a signal to resume.
 *
 * @param index	 The index to replay
 *
 * @return <code>true</code> if the replay should terminate
 **/
static bool check_for_suspend(struct uds_index *index)
{
	bool ret_val;

	if (index->load_context == NULL) {
		return false;
	}

	uds_lock_mutex(&index->load_context->mutex);
	if (index->load_context->status != INDEX_SUSPENDING) {
		uds_unlock_mutex(&index->load_context->mutex);
		return false;
	}

	/* Notify that we are suspended and wait for the resume. */
	index->load_context->status = INDEX_SUSPENDED;
	uds_broadcast_cond(&index->load_context->cond);

	while ((index->load_context->status != INDEX_OPENING) &&
	       (index->load_context->status != INDEX_FREEING)) {
		uds_wait_cond(&index->load_context->cond,
			      &index->load_context->mutex);
	}

	ret_val = (index->load_context->status == INDEX_FREEING);
	uds_unlock_mutex(&index->load_context->mutex);
	return ret_val;
}

/**
 * Replay the volume file to repopulate the volume index.
 *
 * @param index         The index
 *
 * @return              UDS_SUCCESS if successful
 **/
static int replay_volume(struct uds_index *index)
{
	int result;
	unsigned int j, k;
	const struct geometry *geometry;
	uint64_t old_ipm_update, new_ipm_update, vcn;
	uint64_t from_vcn = index->oldest_virtual_chapter;
	uint64_t upto_vcn = index->newest_virtual_chapter;

	uds_log_info("Replaying volume from chapter %llu through chapter %llu",
		     (unsigned long long) from_vcn,
		     (unsigned long long) upto_vcn);

	/*
	 * The index failed to load, so the volume index is empty.  Add records
	 * to the volume index in order, skipping non-hooks in chapters which
	 * will be sparse to save time.
	 *
	 * Go through each record page of each chapter and add the records back
	 * to the volume index.	 This should not cause anything to be written
	 * to either the open chapter or on disk volume.  Also skip the on disk
	 * chapter corresponding to upto_vcn, as this would have already been
	 * purged from the volume index when the chapter was opened.
	 *
	 * Also, go through each index page for each chapter and rebuild the
	 * index page map.
	 */
	geometry = index->volume->geometry;
	old_ipm_update = index->volume->index_page_map->last_update;
	for (vcn = from_vcn; vcn < upto_vcn; ++vcn) {
		bool will_be_sparse_chapter;
		unsigned int chapter;
#ifdef TEST_INTERNAL
		/*
		 * We may be synchronizing with a test waiting for a chapter to
		 * be rebuilt, so we need a memory barrier here.
		 */
		smp_mb__before_atomic();
		atomic_inc(&chapters_replayed);
#endif /* TEST_INTERNAL */
		if (check_for_suspend(index)) {
			uds_log_info("Replay interrupted by index shutdown at chapter %llu",
				     (unsigned long long) vcn);
			return -EBUSY;
		}

		will_be_sparse_chapter =
			is_chapter_sparse(geometry, from_vcn, upto_vcn, vcn);
		chapter = map_to_physical_chapter(geometry, vcn);
		prefetch_volume_pages(&index->volume->volume_store,
				      map_to_physical_page(geometry, chapter, 0),
				      geometry->pages_per_chapter);
		set_volume_index_open_chapter(index->volume_index, vcn);
		result = rebuild_index_page_map(index, vcn);
		if (result != UDS_SUCCESS) {
			return uds_log_error_strerror(result,
						      "could not rebuild index page map for chapter %u",
						      chapter);
		}

		for (j = 0; j < geometry->record_pages_per_chapter; j++) {
			byte *record_page;
			unsigned int record_page_number =
				geometry->index_pages_per_chapter + j;
			result = get_volume_page(index->volume,
						 chapter,
						 record_page_number,
						 &record_page,
						 NULL);
			if (result != UDS_SUCCESS) {
				return uds_log_error_strerror(result,
							      "could not get page %d",
							      record_page_number);
			}
			for (k = 0; k < geometry->records_per_page; k++) {
				const byte *name_bytes =
					record_page + (k * BYTES_PER_RECORD);

				struct uds_chunk_name name;

				memcpy(&name.name, name_bytes,
				       UDS_CHUNK_NAME_SIZE);

				result = replay_record(index, &name, vcn,
						       will_be_sparse_chapter);
				if (result != UDS_SUCCESS) {
					return result;
				}
			}
		}
	}

	/* We also need to reap the chapter being replaced by the open chapter */
	set_volume_index_open_chapter(index->volume_index, upto_vcn);

	new_ipm_update = index->volume->index_page_map->last_update;
	if (new_ipm_update != old_ipm_update) {
		uds_log_info("replay changed index page map update from %llu to %llu",
			     (unsigned long long) old_ipm_update,
			     (unsigned long long) new_ipm_update);
	}

	return UDS_SUCCESS;
}

static int rebuild_index(struct uds_index *index)
{
	/* Find the volume chapter boundaries */
	int result;
	unsigned int i;
	uint64_t lowest_vcn, highest_vcn;
	bool is_empty = false;

	index->volume->lookup_mode = LOOKUP_FOR_REBUILD;
	result = find_volume_chapter_boundaries(index->volume, &lowest_vcn,
						&highest_vcn, &is_empty);
	if (result != UDS_SUCCESS) {
		return uds_log_fatal_strerror(result,
					      "cannot rebuild index: unknown volume chapter boundaries");
	}
	if (lowest_vcn > highest_vcn) {
		uds_log_fatal("cannot rebuild index: no valid chapters exist");
		return UDS_CORRUPT_DATA;
	}

	if (is_empty) {
		index->newest_virtual_chapter =
			index->oldest_virtual_chapter = 0;
	} else {
		unsigned int num_chapters =
			index->volume->geometry->chapters_per_volume;
		index->newest_virtual_chapter = highest_vcn + 1;
		index->oldest_virtual_chapter = lowest_vcn;
		if (index->newest_virtual_chapter ==
		    (index->oldest_virtual_chapter + num_chapters)) {
			/* skip the chapter shadowed by the open chapter */
			index->oldest_virtual_chapter++;
		}
	}

	if ((index->newest_virtual_chapter - index->oldest_virtual_chapter) >
	    index->volume->geometry->chapters_per_volume) {
		return uds_log_fatal_strerror(UDS_CORRUPT_DATA,
					      "cannot rebuild index: volume chapter boundaries too large");
	}

	if (is_empty) {
		set_volume_index_open_chapter(index->volume_index, 0);
		index->volume->lookup_mode = LOOKUP_NORMAL;
		return UDS_SUCCESS;
	}

	result = replay_volume(index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (i = 0; i < index->zone_count; i++) {
		set_active_chapters(index->zones[i]);
	}

	index->volume->lookup_mode = LOOKUP_NORMAL;
	return UDS_SUCCESS;
}

/**
 * Clean up an index zone.
 *
 * @param zone The index zone to free
 **/
static void free_index_zone(struct index_zone *zone)
{
	if (zone == NULL) {
		return;
	}

	free_open_chapter(zone->open_chapter);
	free_open_chapter(zone->writing_chapter);
	UDS_FREE(zone);
}

/**
 * Allocate an index zone.
 *
 * @param index        The index receiving the zone
 * @param zone_number  The number of the zone to allocate
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int make_index_zone(struct uds_index *index, unsigned int zone_number)
{
	struct index_zone *zone;
	int result = UDS_ALLOCATE(1, struct index_zone, "index zone", &zone);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_open_chapter(index->volume->geometry,
				   index->zone_count,
				   &zone->open_chapter);
	if (result != UDS_SUCCESS) {
		free_index_zone(zone);
		return result;
	}

	result = make_open_chapter(index->volume->geometry,
				   index->zone_count,
				   &zone->writing_chapter);
	if (result != UDS_SUCCESS) {
		free_index_zone(zone);
		return result;
	}

	zone->index = index;
	zone->id = zone_number;
	index->zones[zone_number] = zone;

	return UDS_SUCCESS;
}

/**
 * Construct a new index from the given configuration.
 *
 * @param config     The configuration to use
 * @param new        Whether this is a newly formatted index
 * @param new_index  A pointer to hold a pointer to the new index
 *
 * @return UDS_SUCCESS or an error code
 **/
static int allocate_index(struct configuration *config,
			  bool new,
			  struct uds_index **new_index)
{
	struct uds_index *index;
	uint64_t nonce;
	int result;
	unsigned int i;

	result = UDS_ALLOCATE_EXTENDED(struct uds_index,
				       config->zone_count,
				       struct uds_request_queue *,
				       "index",
				       &index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	index->zone_count = config->zone_count;

	result = make_uds_index_layout(config, new, &index->layout);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = UDS_ALLOCATE(index->zone_count, struct index_zone *, "zones",
			      &index->zones);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_volume(config, index->layout, &index->volume);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}
	index->volume->lookup_mode = LOOKUP_NORMAL;

	for (i = 0; i < index->zone_count; i++) {
		result = make_index_zone(index, i);
		if (result != UDS_SUCCESS) {
			free_index(index);
			return uds_log_error_strerror(result,
						      "Could not create index zone");
		}
	}

	nonce = get_uds_volume_nonce(index->layout);
	result = make_volume_index(config, nonce, &index->volume_index);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return uds_log_error_strerror(result,
					      "could not make volume index");
	}

	*new_index = index;
	return UDS_SUCCESS;
}

int make_index(struct configuration *config,
	       enum uds_open_index_type open_type,
	       struct index_load_context *load_context,
	       index_callback_t callback,
	       struct uds_index **new_index)
{
	int result;
	bool loaded = false;
	struct uds_index *index = NULL;

	result = allocate_index(config, (open_type == UDS_CREATE), &index);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "could not allocate index");
	}

	index->load_context = load_context;
	index->callback = callback;

	result = initialize_index_queues(index, config->geometry);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	result = make_chapter_writer(index, &index->chapter_writer);
	if (result != UDS_SUCCESS) {
		free_index(index);
		return result;
	}

	if (open_type == UDS_CREATE) {
		discard_index_state_data(index->layout);
	} else {
		result = load_index(index);
		switch (result) {
		case UDS_SUCCESS:
			loaded = true;
			break;
		case -ENOMEM:
			/* We should not try a rebuild for this error. */
			uds_log_error_strerror(result,
					       "index could not be loaded");
			break;
		default:
			uds_log_error_strerror(result,
					       "index could not be loaded");
			if (open_type == UDS_LOAD) {
				result = rebuild_index(index);
				if (result != UDS_SUCCESS) {
					uds_log_error_strerror(result,
							       "index could not be rebuilt");
				}
			}
			break;
		}
	}

	if (result != UDS_SUCCESS) {
		free_index(index);
		return uds_log_error_strerror(result,
					      "fatal error in make_index");
	}

	if (index->load_context != NULL) {
		uds_lock_mutex(&index->load_context->mutex);
		index->load_context->status = INDEX_READY;
		/*
		 * If we get here, suspend is meaningless, but notify any
		 * thread trying to suspend us so it doesn't hang.
		 */
		uds_broadcast_cond(&index->load_context->cond);
		uds_unlock_mutex(&index->load_context->mutex);
	}

	index->has_saved_open_chapter = loaded;
	index->need_to_save = !loaded;
	*new_index = index;
	return UDS_SUCCESS;
}

void free_index(struct uds_index *index)
{
	unsigned int i;

	if (index == NULL) {
		return;
	}

	uds_request_queue_finish(index->triage_queue);
	for (i = 0; i < index->zone_count; i++) {
		uds_request_queue_finish(index->zone_queues[i]);
	}

	free_chapter_writer(index->chapter_writer);

	if (index->volume_index != NULL) {
		free_volume_index(index->volume_index);
	}

	if (index->zones != NULL) {
		for (i = 0; i < index->zone_count; i++) {
			free_index_zone(index->zones[i]);
		}
		UDS_FREE(index->zones);
	}

	free_volume(index->volume);
	free_uds_index_layout(UDS_FORGET(index->layout));
	UDS_FREE(index);
}

void wait_for_idle_index(struct uds_index *index)
{
	struct chapter_writer *writer = index->chapter_writer;

	uds_lock_mutex(&writer->mutex);
	while (writer->zones_to_write > 0) {
		/*
		 * The chapter writer is probably writing a chapter.  If it is
		 * not, it will soon wake up and write a chapter.
		 */
		uds_wait_cond(&writer->cond, &writer->mutex);
	}
	uds_unlock_mutex(&writer->mutex);
}

int save_index(struct uds_index *index)
{
	int result;

	if (!index->need_to_save) {
		return UDS_SUCCESS;
	}
	wait_for_idle_index(index);
	index->prev_save = index->last_save;
	index->last_save = ((index->newest_virtual_chapter == 0) ?
			    NO_LAST_SAVE :
			    index->newest_virtual_chapter - 1);
	uds_log_info("beginning save (vcn %llu)",
		     (unsigned long long) index->last_save);

	result = save_index_state(index->layout, index);
	if (result != UDS_SUCCESS) {
		uds_log_info("save index failed");
		index->last_save = index->prev_save;
	} else {
		index->has_saved_open_chapter = true;
		index->need_to_save = false;
		uds_log_info("finished save (vcn %llu)",
			     (unsigned long long) index->last_save);
	}
	return result;
}

int replace_index_storage(struct uds_index *index, const char *path)
{
	return replace_volume_storage(index->volume, index->layout, path);
}

void get_index_stats(struct uds_index *index, struct uds_index_stats *counters)
{
	/*
	 * We're accessing the volume index while not on a zone thread, but
	 * that's safe to do when acquiring statistics.
	 */
	struct volume_index_stats dense_stats, sparse_stats;

	get_volume_index_stats(index->volume_index, &dense_stats,
			       &sparse_stats);

	counters->entries_indexed =
		(dense_stats.record_count + sparse_stats.record_count);
	counters->memory_used =
		((uint64_t) dense_stats.memory_allocated +
		 (uint64_t) sparse_stats.memory_allocated +
		 (uint64_t) get_cache_size(index->volume) +
		 index->chapter_writer->memory_allocated);
	counters->collisions =
		(dense_stats.collision_count + sparse_stats.collision_count);
	counters->entries_discarded =
		(dense_stats.discard_count + sparse_stats.discard_count);
}

struct uds_request_queue *select_index_queue(struct uds_index *index,
					     struct uds_request *request,
					     enum request_stage next_stage)
{
	switch (next_stage) {
	case STAGE_TRIAGE:
		/*
		 * The triage queue is only needed for multi-zone sparse
		 * indexes and won't be allocated by the index if not needed,
		 * so simply check for NULL.
		 */
		if (index->triage_queue != NULL) {
			return index->triage_queue;
		}
		/*
		 * Dense index or single zone, so route it directly to the zone
		 * queue.
		 */
		fallthrough;

	case STAGE_INDEX:
		request->zone_number =
			get_volume_index_zone(index->volume_index,
					      &request->chunk_name);
		fallthrough;

	case STAGE_MESSAGE:
		return index->zone_queues[request->zone_number];

	default:
		ASSERT_LOG_ONLY(false, "invalid index stage: %d", next_stage);
	}

	return NULL;
}
