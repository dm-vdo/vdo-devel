/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_H
#define INDEX_H

#include "index-layout.h"
#include "index-session.h"
#include "open-chapter-zone.h"
#include "request.h"
#include "volume.h"
#include "volume-index-ops.h"

#ifdef TEST_INTERNAL
extern atomic_t chapters_replayed;
extern atomic_t chapters_written;
#endif /* TEST_INTERNAL */

/**
 * Callback after a query, update or remove request completes and fills in
 * select fields in the request: status for all requests, oldMetadata and
 * hashExists for query and update requests.
 *
 * @param request  request object
 **/
typedef void (*index_callback_t)(struct uds_request *request);

struct index_zone {
	struct uds_index *index;
	struct open_chapter_zone *open_chapter;
	struct open_chapter_zone *writing_chapter;
	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;
	unsigned int id;
};

struct uds_index {
	bool has_saved_open_chapter;
	bool need_to_save;
	struct index_load_context *load_context;
	struct index_layout *layout;
	struct volume_index *volume_index;
	struct volume *volume;
	unsigned int zone_count;
	struct index_zone **zones;

	/*
	 * ATTENTION!!!
	 * The meaning of the next two fields has changed.
	 *
	 * They now represent the oldest and newest chapters only at load time,
	 * and when the index is quiescent. At other times, they may lag
	 * individual zones' views of the index depending upon the progress
	 * made by the chapter writer.
	 */
	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;

	uint64_t last_save;
	uint64_t prev_save;
	struct chapter_writer *chapter_writer;

	index_callback_t callback;
	struct uds_request_queue *triage_queue;
	struct uds_request_queue *zone_queues[];
};

/**
 * Construct a new index from the given configuration.
 *
 * @param config	The configuration to use
 * @param open_type	How to create the index
 * @param load_context	The load context to use
 * @param callback      The function to invoke when a request completes
 * @param new_index	A pointer to hold a pointer to the new index
 *
 * @return	   UDS_SUCCESS or an error code
 **/
int __must_check make_index(struct configuration *config,
			    enum uds_open_index_type open_type,
			    struct index_load_context *load_context,
			    index_callback_t callback,
			    struct uds_index **new_index);

/**
 * Save an index. The caller must ensure that there are no index requests in
 * progress.
 *
 * @param index   The index to save
 *
 * @return	  UDS_SUCCESS if successful
 **/
int __must_check save_index(struct uds_index *index);

/**
 * Clean up the index and its memory.
 *
 * @param index	  The index to destroy.
 **/
void free_index(struct uds_index *index);

/**
 * Replace the existing index backing store with a different one.
 *
 * @param index  The index
 * @param path   The path to the new backing store
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check replace_index_storage(struct uds_index *index,
				       const char *path);

/**
 * Gather statistics from the volume index, volume, and cache.
 *
 * @param index	    The index
 * @param counters  the statistic counters for the index
 **/
void get_index_stats(struct uds_index *index,
		     struct uds_index_stats *counters);

/**
 * Select and return the request queue responsible for executing the next
 * index stage of a request, updating the request with any associated state
 * (such as the zone number).
 *
 * @param index       The index.
 * @param request     The request destined for the queue.
 * @param next_stage  The next request stage.
 *
 * @return the next index stage queue (the triage queue or the zone queue)
 **/
struct uds_request_queue *select_index_queue(struct uds_index *index,
					     struct uds_request *request,
					     enum request_stage next_stage);

/**
 * Wait for the index to finish all operations that access a local storage
 * device.
 *
 * @param index  The index
 **/
void wait_for_idle_index(struct uds_index *index);

#endif /* INDEX_H */
