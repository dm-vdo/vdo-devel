// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "recovery.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "encodings.h"
#include "heap.h"
#include "int-map.h"
#include "io-submitter.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "wait-queue.h"

/*
 * An explicitly numbered block mapping. Numbering the mappings allows them to be sorted by logical
 * block number during recovery while still preserving the relative order of journal entries with
 * the same logical block number.
 */
struct numbered_block_mapping {
	struct block_map_slot block_map_slot;
	struct block_map_entry block_map_entry;
	/* A serial number to use during replay */
	u32 number;
} __packed;

/*
 * A structure to manage recovering the block map from the recovery journal.
 *
 * Note that the page completions kept in this structure are not immediately freed, so the
 * corresponding pages will be locked down in the page cache until the recovery frees them.
 */
struct block_map_recovery_completion {
	struct vdo_completion completion;
	bool launching;

	/* Fields for the journal entries. */
	struct numbered_block_mapping *journal_entries;
	/*
	 * a heap wrapping journal_entries. It re-orders and sorts journal entries in ascending LBN
	 * order, then original journal order. This permits efficient iteration over the journal
	 * entries in order.
	 */
	struct heap replay_heap;

	/* Fields tracking progress through the journal entries. */

	struct numbered_block_mapping *current_entry;
	/* next entry for which the block map page has not been requested */
	struct numbered_block_mapping *current_unfetched_entry;

	/* Fields tracking requested pages. */
	/* current page's absolute PBN */
	physical_block_number_t pbn;
	page_count_t outstanding;
	page_count_t page_count;
	struct vdo_page_completion page_completions[];
};

struct journal_loader {
	struct vdo_completion *parent;
	data_vio_count_t count;
	data_vio_count_t complete;
	char *journal_data;
	struct vio *vios[];
};

/*
 * The absolute position of an entry in the recovery journal, including the sector number and the
 * entry number within the sector.
 */
struct recovery_point {
	/* Block sequence number */
	sequence_number_t sequence_number;
	/* Sector number */
	u8 sector_count;
	/* Entry number */
	journal_entry_count_t entry_count;
	/* Whether or not the increment portion of the current entry has been applied */
	bool increment_applied;
};

struct recovery_completion {
	/* The completion header */
	struct vdo_completion completion;
	/* A buffer to hold the data read off disk */
	char *journal_data;

	/* The entry data for the block map recovery */
	struct numbered_block_mapping *entries;
	/* The number of entries in the entry array */
	size_t entry_count;
	/* The number of entries to be applied to the block map */
	size_t block_map_entry_count;
	/* The sequence number of the first valid block for block map recovery */
	sequence_number_t block_map_head;
	/* The sequence number of the first valid block for slab journal replay */
	sequence_number_t slab_journal_head;
	/* The sequence number of the last valid block of the journal (if known) */
	sequence_number_t tail;
	/*
	 * The highest sequence number of the journal, not the same as the tail, since the tail
	 * ignores blocks after the first hole.
	 */
	sequence_number_t highest_tail;

	/* A location just beyond the last valid entry of the journal */
	struct recovery_point tail_recovery_point;
	/* The location of the next recovery journal entry to apply */
	struct recovery_point next_recovery_point;
	/* The number of logical blocks currently known to be in use */
	block_count_t logical_blocks_used;
	/* The number of block map data blocks known to be allocated */
	block_count_t block_map_data_blocks;
	/* The journal point to give to the next synthesized decref */
	struct journal_point next_journal_point;
	/* The number of entries played into slab journals */
	size_t entries_added_to_slab_journals;
};

struct rebuild_completion {
	/* The completion header */
	struct vdo_completion completion;

	/* These fields are used for playing the journal into the block map */
	/* A buffer to hold the data read off disk */
	char *journal_data;
	/* The entry data for the block map rebuild */
	struct numbered_block_mapping *entries;
	/* The number of entries in the entry array */
	size_t entry_count;
	/* The sequence number of the first valid block of the journal (if known) */
	sequence_number_t head;
	/* The sequence number of the last valid block of the journal (if known) */
	sequence_number_t tail;

	/* These fields are used for rebuilding the reference counts from the block map. */
	/* The number of logical blocks in use */
	block_count_t logical_blocks_used;
	/* The number of allocated block map pages */
	block_count_t block_map_data_blocks;
	/* the thread on which all block map operations must be done */
	thread_id_t logical_thread_id;
	/* the admin thread */
	thread_id_t admin_thread_id;
	/* the next page to fetch */
	page_count_t page_to_fetch;
	/* the number of leaf pages in the block map */
	page_count_t leaf_pages;
	/* the last slot of the block map */
	struct block_map_slot last_slot;
	/* number of pending (non-ready) requests*/
	page_count_t outstanding;
	/* number of page completions */
	page_count_t page_count;
	/* array of requested, potentially ready page completions */
	struct vdo_page_completion page_completions[];
};

/*
 * This is a heap_comparator function that orders numbered_block_mappings using the
 * 'block_map_slot' field as the primary key and the mapping 'number' field as the secondary key.
 * Using the mapping number preserves the journal order of entries for the same slot, allowing us
 * to sort by slot while still ensuring we replay all entries with the same slot in the exact order
 * as they appeared in the journal.
 *
 * The comparator order is reversed from the usual sense since the heap structure is a max-heap,
 * returning larger elements before smaller ones, but we want to pop entries off the heap in
 * ascending LBN order.
 */
static int compare_mappings(const void *item1, const void *item2)
{
	const struct numbered_block_mapping *mapping1 =
		(const struct numbered_block_mapping *) item1;
	const struct numbered_block_mapping *mapping2 =
		(const struct numbered_block_mapping *) item2;

	if (mapping1->block_map_slot.pbn != mapping2->block_map_slot.pbn)
		return ((mapping1->block_map_slot.pbn < mapping2->block_map_slot.pbn) ? 1 : -1);

	if (mapping1->block_map_slot.slot != mapping2->block_map_slot.slot)
		return ((mapping1->block_map_slot.slot < mapping2->block_map_slot.slot) ? 1 : -1);

	if (mapping1->number != mapping2->number)
		return ((mapping1->number < mapping2->number) ? 1 : -1);

	return 0;
}

/* Implements heap_swapper. */
static void swap_mappings(void *item1, void *item2)
{
	struct numbered_block_mapping *mapping1 = item1;
	struct numbered_block_mapping *mapping2 = item2;
	struct numbered_block_mapping temp = *mapping1;

	*mapping1 = *mapping2;
	*mapping2 = temp;
}

static inline struct block_map_recovery_completion * __must_check
as_block_map_recovery_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion, VDO_BLOCK_MAP_RECOVERY_COMPLETION);
	return container_of(completion, struct block_map_recovery_completion, completion);
}

static void finish_block_map_recovery(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;

	UDS_FREE(as_block_map_recovery_completion(UDS_FORGET(completion)));
	vdo_fail_completion(parent, result);
}

static int make_recovery_completion(struct vdo *vdo,
				    block_count_t entry_count,
				    struct numbered_block_mapping *journal_entries,
				    struct vdo_completion *parent,
				    struct block_map_recovery_completion **recovery_ptr)
{
	page_count_t page_count =
		min(vdo->device_config->cache_size >> 1,
		    (page_count_t) MAXIMUM_SIMULTANEOUS_VDO_BLOCK_MAP_RESTORATION_READS);
	struct block_map_recovery_completion *recovery;
	int result;

	result = UDS_ALLOCATE_EXTENDED(struct block_map_recovery_completion,
				       page_count,
				       struct vdo_page_completion,
				       __func__,
				       &recovery);
	if (result != UDS_SUCCESS)
		return result;

	vdo_initialize_completion(&recovery->completion, vdo, VDO_BLOCK_MAP_RECOVERY_COMPLETION);
	recovery->journal_entries = journal_entries;
	recovery->page_count = page_count;
	recovery->current_entry = &recovery->journal_entries[entry_count - 1];

	/*
	 * Organize the journal entries into a binary heap so we can iterate over them in sorted
	 * order incrementally, avoiding an expensive sort call.
	 */
	initialize_heap(&recovery->replay_heap,
			compare_mappings,
			swap_mappings,
			journal_entries,
			entry_count,
			sizeof(struct numbered_block_mapping));
	build_heap(&recovery->replay_heap, entry_count);
	vdo_prepare_completion(&recovery->completion,
			       finish_block_map_recovery,
			       finish_block_map_recovery,
			       vdo_get_logical_zone_thread(vdo->thread_config, 0),
			       parent);

#ifdef INTERNAL
	/* This message must be recognizable by VDOTest::RebuildBase. */
#endif
	uds_log_info("Replaying %zu recovery entries into block map", recovery->replay_heap.count);

	*recovery_ptr = recovery;
	return VDO_SUCCESS;
}

static void flush_block_map(struct vdo_completion *completion)
{
	thread_id_t thread_id = vdo_get_logical_zone_thread(completion->vdo->thread_config, 0);

	uds_log_info("Flushing block map changes");
	vdo_set_completion_callback(completion, finish_block_map_recovery, thread_id);
	vdo_drain_block_map(completion->vdo->block_map, VDO_ADMIN_STATE_RECOVERING, completion);
}

/* Return: true if recovery is done.  */
static bool finish_if_done(struct block_map_recovery_completion *recovery)
{
	/* Pages are still being launched or there is still work to do */
	if (recovery->launching || (recovery->outstanding > 0))
		return false;

	if (recovery->completion.result != VDO_SUCCESS) {
		/*
		 * We need to be careful here to only free completions that exist. But since we
		 * know none are outstanding, we just go through the ready ones.
		 */
		size_t i;

		for (i = 0; i < recovery->page_count; i++) {
			struct vdo_page_completion *page_completion =
				&recovery->page_completions[i];

			if (recovery->page_completions[i].ready)
				vdo_release_page_completion(&page_completion->completion);
		}
		vdo_finish_completion(&recovery->completion);
		return true;
	}

	if (recovery->current_entry >= recovery->journal_entries)
		return false;

	vdo_launch_completion_callback(&recovery->completion,
				       flush_block_map,
				       recovery->completion.vdo->thread_config->admin_thread);
	return true;
}

static void abort_block_map_recovery(struct block_map_recovery_completion *recovery, int result)
{
	vdo_set_completion_result(&recovery->completion, result);
	finish_if_done(recovery);
}

/**
 * find_entry_starting_next_page() - Find the first journal entry after a given entry which is not
 *                                   on the same block map page.
 * @current_entry: The entry to search from.
 * @needs_sort: Whether sorting is needed to proceed.
 *
 * Return: Pointer to the first later journal entry on a different block map page, or a pointer to
 *         just before the journal entries if no subsequent entry is on a different block map page.
 */
static struct numbered_block_mapping *
find_entry_starting_next_page(struct block_map_recovery_completion *recovery,
			      struct numbered_block_mapping *current_entry,
			      bool needs_sort)
{
	size_t current_page;

	/* If current_entry is invalid, return immediately. */
	if (current_entry < recovery->journal_entries)
		return current_entry;
	current_page = current_entry->block_map_slot.pbn;

	/* Decrement current_entry until it's out of bounds or on a different page. */
	while ((current_entry >= recovery->journal_entries) &&
	       (current_entry->block_map_slot.pbn == current_page)) {
		if (needs_sort) {
			struct numbered_block_mapping *just_sorted_entry =
				sort_next_heap_element(&recovery->replay_heap);
			ASSERT_LOG_ONLY(just_sorted_entry < current_entry,
					"heap is returning elements in an unexpected order");
		}
		current_entry--;
	}
	return current_entry;
}

/*
 * Apply a range of journal entries [starting_entry, ending_entry) journal
 * entries to a block map page.
 */
static void apply_journal_entries_to_page(struct block_map_page *page,
					  struct numbered_block_mapping *starting_entry,
					  struct numbered_block_mapping *ending_entry)
{
	struct numbered_block_mapping *current_entry = starting_entry;

	while (current_entry != ending_entry) {
		page->entries[current_entry->block_map_slot.slot] = current_entry->block_map_entry;
		current_entry--;
	}
}

static void recover_ready_pages(struct block_map_recovery_completion *recovery,
				struct vdo_completion *completion);

static void block_map_page_loaded(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);

	recovery->outstanding--;
	if (!recovery->launching)
		recover_ready_pages(recovery, completion);
}

static void handle_block_map_page_load_error(struct vdo_completion *completion)
{
	struct block_map_recovery_completion *recovery =
		as_block_map_recovery_completion(completion->parent);

	recovery->outstanding--;
	abort_block_map_recovery(recovery, completion->result);
}

static void fetch_block_map_page(struct block_map_recovery_completion *recovery,
				 struct vdo_completion *completion)
{
	physical_block_number_t new_pbn;

	if (recovery->current_unfetched_entry < recovery->journal_entries)
		/* Nothing left to fetch. */
		return;

	/* Fetch the next page we haven't yet requested. */
	new_pbn = recovery->current_unfetched_entry->block_map_slot.pbn;
	recovery->current_unfetched_entry =
		find_entry_starting_next_page(recovery, recovery->current_unfetched_entry, true);
	recovery->outstanding++;
	vdo_get_page(((struct vdo_page_completion *) completion),
		     &recovery->completion.vdo->block_map->zones[0],
		     new_pbn,
		     true,
		     &recovery->completion,
		     block_map_page_loaded,
		     handle_block_map_page_load_error,
		     false);
}

static struct vdo_page_completion *
get_next_page_completion(struct block_map_recovery_completion *recovery,
			 struct vdo_page_completion *completion)
{
	completion++;
	if (completion == (&recovery->page_completions[recovery->page_count]))
		completion = &recovery->page_completions[0];
	return completion;
}

static void recover_ready_pages(struct block_map_recovery_completion *recovery,
				struct vdo_completion *completion)
{
	struct vdo_page_completion *page_completion = (struct vdo_page_completion *) completion;

	if (finish_if_done(recovery))
		return;

	if (recovery->pbn != page_completion->pbn)
		return;

	while (page_completion->ready) {
		struct numbered_block_mapping *start_of_next_page;
		struct block_map_page *page;
		int result;

		result = vdo_get_cached_page(completion, &page);
		if (result != VDO_SUCCESS) {
			abort_block_map_recovery(recovery, result);
			return;
		}

		start_of_next_page =
			find_entry_starting_next_page(recovery, recovery->current_entry, false);
		apply_journal_entries_to_page(page, recovery->current_entry, start_of_next_page);
		recovery->current_entry = start_of_next_page;
		vdo_request_page_write(completion);
		vdo_release_page_completion(completion);

		if (finish_if_done(recovery))
			return;

		recovery->pbn = recovery->current_entry->block_map_slot.pbn;
		fetch_block_map_page(recovery, completion);
		page_completion = get_next_page_completion(recovery, page_completion);
		completion = &page_completion->completion;
	}
}

/* Recover the block map (normal rebuild). */
EXTERNAL_STATIC void
recover_block_map(struct vdo *vdo,
		  block_count_t entry_count,
		  struct numbered_block_mapping *journal_entries,
		  struct vdo_completion *parent)
{
	struct numbered_block_mapping *first_sorted_entry;
	page_count_t i;
	struct block_map_recovery_completion *recovery;
	int result;
	thread_id_t thread_id = vdo_get_logical_zone_thread(vdo->thread_config, 0);

	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == thread_id),
			"%s must be called on logical thread %u (not %u)",
			__func__,
			thread_id,
			vdo_get_callback_thread_id());

	result = make_recovery_completion(vdo, entry_count, journal_entries, parent, &recovery);
	if (result != VDO_SUCCESS) {
		vdo_fail_completion(parent, result);
		return;
	}

	if (is_heap_empty(&recovery->replay_heap)) {
		vdo_finish_completion(&recovery->completion);
		return;
	}

	first_sorted_entry = sort_next_heap_element(&recovery->replay_heap);
	ASSERT_LOG_ONLY(first_sorted_entry == recovery->current_entry,
			"heap is returning elements in an unexpected order");

	/* Prevent any page from being processed until all pages have been launched. */
	recovery->launching = true;
	recovery->pbn = recovery->current_entry->block_map_slot.pbn;
	recovery->current_unfetched_entry = recovery->current_entry;
	for (i = 0; i < recovery->page_count; i++) {
		if (recovery->current_unfetched_entry < recovery->journal_entries)
			break;

		fetch_block_map_page(recovery, &recovery->page_completions[i].completion);
	}
	recovery->launching = false;

	/* Process any ready pages. */
	recover_ready_pages(recovery, &recovery->page_completions[0].completion);
}

/**
 * as_vdo_recovery_completion() - Convert a generic completion to a recovery_completion.
 * @completion: The completion to convert.
 *
 * Return: The recovery_completion.
 */
static inline struct recovery_completion * __must_check
as_recovery_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion, VDO_RECOVERY_COMPLETION);
	return container_of(completion, struct recovery_completion, completion);
}

/**
 * is_replaying() - Check whether a vdo was replaying the recovery journal into the block map when
 *                  it crashed.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo crashed while reconstructing the block map.
 */
static bool __must_check is_replaying(const struct vdo *vdo)
{
	return (vdo_get_state(vdo) == VDO_REPLAYING);
}

/**
 * get_recovery_journal_block_header() - Get the block header for a block at a position in the
 *                                       journal data and unpack it.
 * @journal: The recovery journal.
 * @data: The recovery journal data.
 * @sequence: The sequence number.
 *
 * Return: The unpacked header.
 */
static struct recovery_block_header __must_check
get_recovery_journal_block_header(struct recovery_journal *journal,
				  char *data,
				  sequence_number_t sequence)
{
	physical_block_number_t pbn = vdo_get_recovery_journal_block_number(journal, sequence);
	char *header = &data[pbn * VDO_BLOCK_SIZE];

	return vdo_unpack_recovery_block_header((struct packed_journal_header *) header);
}

/**
 * is_valid_recovery_journal_block() - Determine whether the given header describes a valid block
 *                                     for the given journal.
 * @journal: The journal to use.
 * @header: The unpacked block header to check.
 * @old_ok: Whether an old format header is valid.
 *
 * A block is not valid if it is unformatted, or if it is older than the last successful recovery
 * or reformat.
 *
 * Return: True if the header is valid.
 */
static bool __must_check
is_valid_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header,
				bool old_ok)
{
	if ((header->nonce != journal->nonce) ||
	    (header->recovery_count != journal->recovery_count))
		return false;

	if (header->metadata_type == VDO_METADATA_RECOVERY_JOURNAL_2)
		return (header->entry_count <= journal->entries_per_block);

	return (old_ok &&
		(header->metadata_type == VDO_METADATA_RECOVERY_JOURNAL) &&
		(header->entry_count <= RECOVERY_JOURNAL_1_ENTRIES_PER_BLOCK));
}

/**
 * is_exact_recovery_journal_block() - Determine whether the given header describes the exact block
 *                                     indicated.
 * @journal: The journal to use.
 * @header: The unpacked block header to check.
 * @sequence: The expected sequence number.
 * @type: The expected metadata type.
 *
 * Return: True if the block matches.
 */
static bool __must_check
is_exact_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header,
				sequence_number_t sequence,
				enum vdo_metadata_type type)
{
	return ((header->metadata_type == type) &&
		(header->sequence_number == sequence) &&
		(is_valid_recovery_journal_block(journal, header, true)));
}

/**
 * find_recovery_journal_head_and_tail() - Find the tail and head of the journal.
 * @journal: The recovery journal.
 * @journal_data: The journal data read from disk.
 * @tail_ptr: A pointer to return the tail found, or if no higher block is found, the value
 *            currently in the journal.
 * @block_map_head_ptr: A pointer to return the block map head.
 * @slab_journal_head_ptr: An optional pointer to return the slab journal head.
 *
 * Finds the tail and the head of the journal by searching for the highest sequence number in a
 * block with a valid nonce, and the highest head value among the blocks with valid nonces.
 *
 * Return: True if there were valid journal blocks.
 */
static bool find_recovery_journal_head_and_tail(struct recovery_journal *journal,
						char *journal_data,
						sequence_number_t *tail_ptr,
						sequence_number_t *block_map_head_ptr,
						sequence_number_t *slab_journal_head_ptr)
{
	sequence_number_t highest_tail = journal->tail;
	sequence_number_t block_map_head_max = 0;
	sequence_number_t slab_journal_head_max = 0;
	bool found_entries = false;
	physical_block_number_t i;

	for (i = 0; i < journal->size; i++) {
		struct recovery_block_header header =
			get_recovery_journal_block_header(journal, journal_data, i);

		if (!is_valid_recovery_journal_block(journal, &header, true))
			/* This block is old or incorrectly formatted */
			continue;

		if (vdo_get_recovery_journal_block_number(journal, header.sequence_number) != i)
			/* This block is in the wrong location */
			continue;

		if (header.sequence_number >= highest_tail) {
			found_entries = true;
			highest_tail = header.sequence_number;
		}

		if (header.block_map_head > block_map_head_max)
			block_map_head_max = header.block_map_head;

		if (header.slab_journal_head > slab_journal_head_max)
			slab_journal_head_max = header.slab_journal_head;
	}

	*tail_ptr = highest_tail;
	if (!found_entries)
		return false;

	*block_map_head_ptr = block_map_head_max;
	if (slab_journal_head_ptr != NULL)
		*slab_journal_head_ptr = slab_journal_head_max;

	return true;
}

/**
 * increment_recovery_point() - Move the given recovery point forward by one entry.
 * @point: The recovery point to alter.
 */
static void increment_recovery_point(struct recovery_point *point)
{
	if (++point->entry_count < RECOVERY_JOURNAL_ENTRIES_PER_SECTOR)
		return;

	point->entry_count = 0;
	if (point->sector_count < (VDO_SECTORS_PER_BLOCK - 1)) {
		point->sector_count++;
		return;
	}

	point->sequence_number++;
	point->sector_count = 1;
}

/**
 * before_recovery_point() - Check whether the first point precedes the second point.
 * @first: The first recovery point.
 * @second: The second recovery point.
 *
 * Return: true if the first point precedes the second point.
 */
static bool __must_check
before_recovery_point(const struct recovery_point *first, const struct recovery_point *second)
{
	if (first->sequence_number < second->sequence_number)
		return true;

	if (first->sequence_number > second->sequence_number)
		return false;

	if (first->sector_count < second->sector_count)
		return true;

	return ((first->sector_count == second->sector_count) &&
		(first->entry_count < second->entry_count));
}

static void prepare_recovery_completion(struct recovery_completion *recovery,
					vdo_action *callback,
					enum vdo_zone_type zone_type)
{
	struct vdo_completion *completion = &recovery->completion;
	const struct thread_config *thread_config = completion->vdo->thread_config;
	thread_id_t thread_id;

	/* All blockmap access is done on single thread, so use logical zone 0. */
	thread_id = ((zone_type == VDO_ZONE_TYPE_LOGICAL) ?
		     vdo_get_logical_zone_thread(thread_config, 0) :
		     thread_config->admin_thread);
	vdo_reset_completion(completion);
	vdo_set_completion_callback(completion, callback, thread_id);
}

/**
 * free_vdo_recovery_completion() - Free a recovery_completion and all underlying structures.
 * @recovery: The recovery completion to free.
 */
static void free_vdo_recovery_completion(struct recovery_completion *recovery)
{
	if (recovery == NULL)
		return;

	UDS_FREE(UDS_FORGET(recovery->journal_data));
	UDS_FREE(UDS_FORGET(recovery->entries));
	UDS_FREE(recovery);
}

/**
 * finish_recovery() - Finish recovering, free the recovery completion and notify the parent.
 * @completion: The recovery completion.
 */
static void finish_recovery(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;
	u64 recovery_count = ++vdo->states.vdo.complete_recoveries;

	vdo_initialize_recovery_journal_post_recovery(vdo->recovery_journal,
						      recovery_count,
						      recovery->highest_tail);
	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_info("Rebuild complete");

	/*
	 * Now that we've freed the recovery completion and its vast array of journal entries, we
	 * can allocate refcounts.
	 */
	vdo_continue_completion(parent, vdo_allocate_slab_ref_counts(vdo->depot));
}

/**
 * abort_recovery() - Handle a recovery error.
 * @completion: The recovery completion.
 */
static void abort_recovery(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct recovery_completion *recovery = as_recovery_completion(completion);

	free_vdo_recovery_completion(UDS_FORGET(recovery));
	uds_log_warning("Recovery aborted");
	vdo_continue_completion(parent, result);
}

/**
 * abort_recovery_on_error() - Abort a recovery if there is an error.
 * @result: The result to check.
 * @recovery: The recovery completion.
 *
 * Return: true if the result was an error.
 */
static bool __must_check abort_recovery_on_error(int result, struct recovery_completion *recovery)
{
	if (result == VDO_SUCCESS)
		return false;

	vdo_fail_completion(&recovery->completion, result);
	return true;
}

static struct packed_journal_sector * __must_check
get_sector(struct recovery_journal *journal,
	   char *journal_data,
	   sequence_number_t sequence,
	   u8 sector_number)
{
	off_t offset;

	offset = ((vdo_get_recovery_journal_block_number(journal, sequence) * VDO_BLOCK_SIZE) +
		  (VDO_SECTOR_SIZE * sector_number));
	return (struct packed_journal_sector *) (journal_data + offset);
}

/**
 * get_entry() - Unpack the recovery journal entry associated with the given recovery point.
 * @recovery: The recovery completion.
 * @point: The recovery point.
 *
 * Return: The unpacked contents of the matching recovery journal entry.
 */
static struct recovery_journal_entry
get_entry(const struct recovery_completion *recovery, const struct recovery_point *point)
{
	struct packed_journal_sector *sector;

	sector = get_sector(recovery->completion.vdo->recovery_journal,
			    recovery->journal_data,
			    point->sequence_number,
			    point->sector_count);
	return vdo_unpack_recovery_journal_entry(&sector->entries[point->entry_count]);
}

/**
 * validate_recovery_journal_entry() - Validate a recovery journal entry.
 * @vdo: The vdo.
 * @entry: The entry to validate.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int
validate_recovery_journal_entry(const struct vdo *vdo, const struct recovery_journal_entry *entry)
{
	if ((entry->slot.pbn >= vdo->states.vdo.config.physical_blocks) ||
	    (entry->slot.slot >= VDO_BLOCK_MAP_ENTRIES_PER_PAGE) ||
	    !vdo_is_valid_location(&entry->mapping) ||
	    !vdo_is_valid_location(&entry->unmapping) ||
	    !vdo_is_physical_data_block(vdo->depot, entry->mapping.pbn) ||
	    !vdo_is_physical_data_block(vdo->depot, entry->unmapping.pbn))
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: %s (%llu, %u) from %llu to %llu is not within bounds",
					      vdo_get_journal_operation_name(entry->operation),
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->unmapping.pbn,
					      (unsigned long long) entry->mapping.pbn);

	if ((entry->operation == VDO_JOURNAL_BLOCK_MAP_REMAPPING) &&
	    (vdo_is_state_compressed(entry->mapping.state) ||
	     (entry->mapping.pbn == VDO_ZERO_BLOCK) ||
	     (entry->unmapping.state != VDO_MAPPING_STATE_UNMAPPED) ||
	     (entry->unmapping.pbn != VDO_ZERO_BLOCK)))
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: %s (%llu, %u) from %llu to %llu is not a valid tree mapping",
					      vdo_get_journal_operation_name(entry->operation),
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->unmapping.pbn,
					      (unsigned long long) entry->mapping.pbn);

	return VDO_SUCCESS;
}

/**
 * extract_increments() - Create an array of all valid increment entries, in order, and store it in
 *                        the recovery completion.
 * @recovery: The recovery completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int extract_increments(struct recovery_completion *recovery)
{
	int result;
	struct vdo *vdo = recovery->completion.vdo;
	struct recovery_point recovery_point = {
		.sequence_number = recovery->block_map_head,
		.sector_count = 1,
		.entry_count = 0,
	};

	/*
	 * Allocate an array of numbered_block_mapping structs just large enough to transcribe
	 * every packed_recovery_journal_entry from every valid journal block.
	 */
	result = UDS_ALLOCATE(recovery->entry_count,
			      struct numbered_block_mapping,
			      __func__,
			      &recovery->entries);
	if (result != VDO_SUCCESS)
		return result;

	for (; before_recovery_point(&recovery_point, &recovery->tail_recovery_point);
	     increment_recovery_point(&recovery_point)) {
		struct recovery_journal_entry entry = get_entry(recovery, &recovery_point);

		result = validate_recovery_journal_entry(vdo, &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo, result);
			return result;
		}

		recovery->entries[recovery->block_map_entry_count] =
			(struct numbered_block_mapping) {
			.block_map_slot = entry.slot,
			.block_map_entry = vdo_pack_block_map_entry(entry.mapping.pbn,
								    entry.mapping.state),
			.number = recovery->block_map_entry_count,
		};
		recovery->block_map_entry_count++;
	}

	result = ASSERT((recovery->block_map_entry_count <= recovery->entry_count),
			"approximate entry count is an upper bound");
	if (result != VDO_SUCCESS)
		vdo_enter_read_only_mode(vdo, result);

	return result;
}

/**
 * launch_block_map_recovery() - Extract journal entries and recover the block map.
 * @completion: The recovery completion
 *
 * This callback is registered in start_super_block_save().
 */
static void launch_block_map_recovery(struct vdo_completion *completion)
{
	int result;
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	/* Extract the journal entries for the block map recovery. */
	result = extract_increments(recovery);
	if (abort_recovery_on_error(result, recovery))
		return;

	prepare_recovery_completion(recovery, finish_recovery, VDO_ZONE_TYPE_ADMIN);
	recover_block_map(vdo, recovery->block_map_entry_count, recovery->entries, completion);
}

/**
 * start_super_block_save() - Finish flushing all slab journals and start a write of the super
 *                            block.
 * @completion: The recovery completion.
 *
 * This callback is registered in add_synthesized_entries().
 */
static void start_super_block_save(struct vdo_completion *completion)
{
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

	uds_log_info("Saving recovery progress");
	vdo_set_state(vdo, VDO_REPLAYING);

	/*
	 * The block map access which follows the super block save must be done on a logical
	 * thread.
	 */
	prepare_recovery_completion(recovery, launch_block_map_recovery, VDO_ZONE_TYPE_LOGICAL);
	vdo_save_components(vdo, completion);
}

/**
 * finish_recovering_depot() - The callback from loading the slab depot.
 * @completion: The recovery completion.
 *
 * Updates the logical blocks and block map data blocks counts in the recovery journal and then
 * drains the slab depot in order to commit the recovered slab journals. It is registered in
 * apply_to_depot().
 */
static void finish_recovering_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

	uds_log_info("Replayed %zu journal entries into slab journals",
		     recovery->entries_added_to_slab_journals);

	vdo->recovery_journal->logical_blocks_used = recovery->logical_blocks_used;
	vdo->recovery_journal->block_map_data_blocks = recovery->block_map_data_blocks;

	prepare_recovery_completion(recovery, start_super_block_save, VDO_ZONE_TYPE_ADMIN);
	vdo_drain_slab_depot(vdo->depot, VDO_ADMIN_STATE_RECOVERING, completion);
}

/**
 * compute_usages() - Determine the LBNs used count as of the end of the journal.
 * @recovery: The recovery completion.
 *
 * Does not include any changes to that count from entries that will be synthesized later.
 *
 * Return: VDO_SUCCESS or an error.
 */
static noinline int compute_usages(struct recovery_completion *recovery)
{
	/*
	 * VDO-5182: function is declared noinline to avoid what is likely a spurious valgrind
	 * error about this structure being uninitialized.
	 */
	struct recovery_point recovery_point = {
		.sequence_number = recovery->tail,
		.sector_count = 1,
		.entry_count = 0,
	};

	struct vdo *vdo = recovery->completion.vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	struct recovery_block_header header =
		get_recovery_journal_block_header(journal, recovery->journal_data, recovery->tail);

	recovery->logical_blocks_used = header.logical_blocks_used;
	recovery->block_map_data_blocks = header.block_map_data_blocks;

	for (; before_recovery_point(&recovery_point, &recovery->tail_recovery_point);
	     increment_recovery_point(&recovery_point)) {
		struct recovery_journal_entry entry = get_entry(recovery, &recovery_point);
		int result;

		result = validate_recovery_journal_entry(vdo, &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo, result);
			return result;
		}

		if (entry.operation == VDO_JOURNAL_BLOCK_MAP_REMAPPING) {
			recovery->block_map_data_blocks++;
			continue;
		}

		if (vdo_is_mapped_location(&entry.mapping))
			recovery->logical_blocks_used++;

		if (vdo_is_mapped_location(&entry.unmapping))
			recovery->logical_blocks_used--;
	}

	return VDO_SUCCESS;
}

/**
 * advance_points() - Advance the current recovery and journal points.
 * @recovery: The recovery_completion whose points are to be advanced.
 * @entries_per_block: The number of entries in a recovery journal block.
 */
static void
advance_points(struct recovery_completion *recovery, journal_entry_count_t entries_per_block)
{
	if (!recovery->next_recovery_point.increment_applied) {
		recovery->next_recovery_point.increment_applied	= true;
		return;
	}

	increment_recovery_point(&recovery->next_recovery_point);
	vdo_advance_journal_point(&recovery->next_journal_point, entries_per_block);
	recovery->next_recovery_point.increment_applied	= false;
}

/**
 * add_slab_journal_entries() - Replay recovery journal entries into the slab journals of the
 *                              allocator currently being recovered.
 * @completion: The allocator completion.
 *
 * Waits for slab journal tailblock space when necessary. This method is its own callback.
 */
static void add_slab_journal_entries(struct vdo_completion *completion)
{
	struct recovery_point *recovery_point;
	struct recovery_completion *recovery = completion->parent;
	struct vdo *vdo = completion->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	struct block_allocator *allocator = vdo_as_block_allocator(completion);

	/* Get ready in case we need to enqueue again. */
	vdo_prepare_completion(completion,
			       add_slab_journal_entries,
			       vdo_notify_slab_journals_are_recovered,
			       completion->callback_thread_id,
			       recovery);
	for (recovery_point = &recovery->next_recovery_point;
	     before_recovery_point(recovery_point, &recovery->tail_recovery_point);
	     advance_points(recovery, journal->entries_per_block)) {
		int result;
		physical_block_number_t pbn;
		struct vdo_slab *slab;
		struct recovery_journal_entry entry = get_entry(recovery, recovery_point);
		bool increment = !recovery->next_recovery_point.increment_applied;

		if (increment) {
			result = validate_recovery_journal_entry(vdo, &entry);
			if (result != VDO_SUCCESS) {
				vdo_enter_read_only_mode(vdo, result);
				vdo_fail_completion(completion, result);
				return;
			}

			pbn = entry.mapping.pbn;
		} else {
			pbn = entry.unmapping.pbn;
		}

		if (pbn == VDO_ZERO_BLOCK)
			continue;

		slab = vdo_get_slab(vdo->depot, pbn);
		if (slab->allocator != allocator)
			continue;

		if (!vdo_attempt_replay_into_slab_journal(slab->journal,
							  pbn,
							  entry.operation,
							  increment,
							  &recovery->next_journal_point,
							  completion))
			return;

		recovery->entries_added_to_slab_journals++;
	}

	vdo_notify_slab_journals_are_recovered(completion);
}

/**
 * vdo_replay_into_slab_journals() - Replay recovery journal entries in the slab journals of slabs
 *                                   owned by a given block_allocator.
 * @allocator: The allocator whose slab journals are to be recovered.
 * @context: The slab depot load context supplied by a recovery when it loads the depot.
 */
void vdo_replay_into_slab_journals(struct block_allocator *allocator, void *context)
{
	struct vdo_completion *completion = &allocator->completion;
	struct recovery_completion *recovery = context;
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_physical_zone_thread(vdo, allocator->zone_number, __func__);
	if ((recovery->journal_data == NULL) || is_replaying(vdo)) {
		/* there's nothing to replay */
		vdo_notify_slab_journals_are_recovered(completion);
		return;
	}

	recovery->next_recovery_point = (struct recovery_point) {
		.sequence_number = recovery->slab_journal_head,
		.sector_count = 1,
		.entry_count = 0,
	};

	recovery->next_journal_point = (struct journal_point) {
		.sequence_number = recovery->slab_journal_head,
		.entry_count = 0,
	};

	uds_log_info("Replaying entries into slab journals for zone %u", allocator->zone_number);
	completion->parent = recovery;
	add_slab_journal_entries(completion);
}

static bool validate_heads(struct recovery_completion *recovery)
{
	/* Both reap heads must be behind the tail. */
	if ((recovery->block_map_head > recovery->tail) ||
	    (recovery->slab_journal_head > recovery->tail)) {
		int result = uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						    "Journal tail too early. block map head: %llu, slab journal head: %llu, tail: %llu",
						    (unsigned long long) recovery->block_map_head,
						    (unsigned long long) recovery->slab_journal_head,
						    (unsigned long long) recovery->tail);
		vdo_fail_completion(&recovery->completion, result);
		return false;
	}

	return true;
}

/**
 * prepare_to_apply_journal_entries() - Determine the limits of the valid recovery journal and
 *                                      prepare to replay into the slab journals and block map.
 * @recovery: The recovery completion.
 *
 * Return: true if the recovery process has been continued, if not, the caller is responsible for
 *         continuing the process
 */
static bool prepare_to_apply_journal_entries(struct recovery_completion *recovery)
{
	sequence_number_t i, head;
	bool found_entries = false;
	struct recovery_journal *journal = recovery->completion.vdo->recovery_journal;

	if (!find_recovery_journal_head_and_tail(journal,
						 recovery->journal_data,
						 &recovery->highest_tail,
						 &recovery->block_map_head,
						 &recovery->slab_journal_head))
		return false;


	head = min(recovery->block_map_head, recovery->slab_journal_head);
	for (i = head; i <= recovery->highest_tail; i++) {
		struct recovery_block_header header;
		journal_entry_count_t block_entries;
		u8 j;

		recovery->tail = i;
		recovery->tail_recovery_point = (struct recovery_point) {
			.sequence_number = i,
			.sector_count = 0,
			.entry_count = 0,
		};

		header = get_recovery_journal_block_header(journal, recovery->journal_data, i);
		if (header.metadata_type == VDO_METADATA_RECOVERY_JOURNAL) {
			/* This is an old format block, so we need to upgrade */
			uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					       "Recovery journal is in the old format, a read-only rebuild is required.");
			vdo_enter_read_only_mode(recovery->completion.vdo,
						 VDO_UNSUPPORTED_VERSION);
			vdo_fail_completion(&recovery->completion, VDO_READ_ONLY);
			return true;
		}

		if (!is_exact_recovery_journal_block(journal,
						     &header,
						     i,
						     VDO_METADATA_RECOVERY_JOURNAL_2))
			/* A bad block header was found so this must be the end of the journal. */
			break;

		block_entries = header.entry_count;

		/* Examine each sector in turn to determine the last valid sector. */
		for (j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
			struct packed_journal_sector *sector =
				get_sector(journal, recovery->journal_data, i, j);
			journal_entry_count_t sector_entries =
				min_t(journal_entry_count_t, sector->entry_count, block_entries);

			/* A bad sector means that this block was torn. */
			if (!vdo_is_valid_recovery_journal_sector(&header, sector, j))
				break;

			if (sector_entries > 0) {
				found_entries = true;
				recovery->tail_recovery_point.sector_count++;
				recovery->tail_recovery_point.entry_count = sector_entries;
				block_entries -= sector_entries;
				recovery->entry_count += sector_entries;
			}

			/* If this sector is short, the later sectors can't matter. */
			if ((sector_entries < RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) ||
			    (block_entries == 0))
				break;
		}

		/* If this block was not filled, or if it tore, no later block can matter. */
		if ((header.entry_count != journal->entries_per_block) || (block_entries > 0))
			break;
	}

	if (!found_entries)
		return false;

	/* Set the tail to the last valid tail block, if there is one. */
	if (recovery->tail_recovery_point.sector_count == 0)
		recovery->tail--;

	if (!validate_heads(recovery))
		return true;

	uds_log_info("Highest-numbered recovery journal block has sequence number %llu, and the highest-numbered usable block is %llu",
		     (unsigned long long) recovery->highest_tail,
		     (unsigned long long) recovery->tail);

	if (is_replaying(recovery->completion.vdo)) {
		prepare_recovery_completion(recovery,
					    launch_block_map_recovery,
					    VDO_ZONE_TYPE_LOGICAL);
	} else {
		prepare_recovery_completion(recovery,
					    finish_recovering_depot,
					    VDO_ZONE_TYPE_ADMIN);
		if (abort_recovery_on_error(compute_usages(recovery), recovery))
			return true;
	}

	vdo_load_slab_depot(recovery->completion.vdo->depot,
			    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
			    &recovery->completion,
			    recovery);
	return true;
}

/**
 * launch_recovery() - Construct a recovery completion and launch it.
 * @parent: The completion to notify when the offline portion of the recovery is complete.
 * @journal_data: The contents of the recovery journal
 *
 * Applies all valid journal block entries to all vdo structures. This function performs the
 * offline portion of recovering a vdo from a crash.
 */
static void launch_recovery(struct vdo_completion *parent, char *journal_data)
{
	struct vdo *vdo = parent->vdo;
	struct recovery_completion *recovery;
	int result;

	result = UDS_ALLOCATE(1, struct recovery_completion, __func__, &recovery);
	if (result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		vdo_fail_completion(parent, result);
		return;
	}

	recovery->journal_data = journal_data;
	vdo_initialize_completion(&recovery->completion, vdo, VDO_RECOVERY_COMPLETION);
	recovery->completion.error_handler = abort_recovery;
	recovery->completion.parent = parent;
	prepare_recovery_completion(recovery, finish_recovery, VDO_ZONE_TYPE_ADMIN);

	if (prepare_to_apply_journal_entries(recovery) || !validate_heads(recovery))
		return;

	/* This message must be in sync with VDOTest::RebuildBase. */
	uds_log_info("Replaying 0 recovery entries into block map");
	/* We still need to load the slab_depot. */
	UDS_FREE(UDS_FORGET(recovery->journal_data));
	vdo_load_slab_depot(vdo->depot,
			    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
			    &recovery->completion,
			    recovery);
}

/*--------------------------------------------------------------------*/

/** as_rebuild_completion() - Convert a generic completion to a rebuild_completion. */
static inline struct rebuild_completion * __must_check
as_rebuild_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion, VDO_READ_ONLY_REBUILD_COMPLETION);
	return container_of(completion, struct rebuild_completion, completion);
}

/**
 * free_rebuild_completion() - Free a rebuild completion and all underlying structures.
 * @rebuild: The rebuild completion to free.
 */
static void free_rebuild_completion(struct rebuild_completion *rebuild)
{
	if (rebuild == NULL)
		return;

	UDS_FREE(UDS_FORGET(rebuild->journal_data));
	UDS_FREE(UDS_FORGET(rebuild->entries));
	UDS_FREE(rebuild);
}

/**
 * complete_rebuild() - Clean up the rebuild process.
 * @completion: The rebuild completion.
 *
 * Cleans up the rebuild process, whether or not it succeeded, by freeing the rebuild completion and
 * notifying the parent of the outcome.
 */
static void complete_rebuild(struct vdo_completion *completion)
{
	struct vdo_completion *parent = completion->parent;
	int result = completion->result;
	struct block_map *block_map = completion->vdo->block_map;
	struct rebuild_completion *rebuild = as_rebuild_completion(UDS_FORGET(completion));

	block_map->zones[0].page_cache.rebuilding = false;
	free_rebuild_completion(UDS_FORGET(rebuild));
	vdo_continue_completion(parent, result);
}

/**
 * finish_read_only_rebuild() - Finish rebuilding, free the rebuild completion and notify the
 *                              parent.
 * @completion: The rebuild_completion.
 */
static void finish_read_only_rebuild(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

	if (vdo->load_state != VDO_REBUILD_FOR_UPGRADE)
		/* A "rebuild" for upgrade should not increment this count. */
		vdo->states.vdo.complete_recoveries++;

	vdo_initialize_recovery_journal_post_rebuild(vdo->recovery_journal,
						     vdo->states.vdo.complete_recoveries,
						     rebuild->tail,
						     rebuild->logical_blocks_used,
						     rebuild->block_map_data_blocks);
	uds_log_info("Read-only rebuild complete");
	complete_rebuild(completion);
}

/**
 * abort_rebuild() - Handle a rebuild error.
 * @completion: The rebuild_completion.
 */
static void abort_rebuild(struct vdo_completion *completion)
{
	uds_log_info("Read-only rebuild aborted");
	complete_rebuild(completion);
}

static void prepare_rebuild_completion(struct rebuild_completion *rebuild,
				       vdo_action *callback,
				       thread_id_t callback_thread_id)
{
	vdo_reset_completion(&rebuild->completion);
	vdo_set_completion_callback(&rebuild->completion, callback, callback_thread_id);
}

/**
 * abort_rebuild_on_error() - Abort a rebuild if there is an error.
 * @result: The result to check.
 * @rebuild: The rebuild completion.
 *
 * Return: true if the result was an error.
 */
static bool __must_check abort_rebuild_on_error(int result, struct rebuild_completion *rebuild)
{
	if (result == VDO_SUCCESS)
		return false;

	vdo_fail_completion(&rebuild->completion, result);
	return true;
}

/**
 * drain_slab_depot() - Flush out all dirty refcounts blocks now that they have been rebuilt.
 * @completion: The rebuild completion.
 *
 * This callback is registered in flush_block_map_updates().
 */
static void drain_slab_depot(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_admin_thread(vdo, __func__);

	uds_log_info("Saving rebuilt state");
	prepare_rebuild_completion(as_rebuild_completion(completion),
				   finish_read_only_rebuild,
				   completion->callback_thread_id);
	vdo_drain_slab_depot(vdo->depot, VDO_ADMIN_STATE_REBUILDING, completion);
}

/**
 * flush_block_map_updates() - Flush the block map now that all the reference counts are rebuilt.
 * @completion: The rebuild completion.
 *
 * This callback is registered in finish_if_done().
 */
static void flush_block_map_updates(struct vdo_completion *completion)
{
	vdo_assert_on_admin_thread(completion->vdo, __func__);

	uds_log_info("Flushing block map changes");
	prepare_rebuild_completion(as_rebuild_completion(completion),
				   drain_slab_depot,
				   completion->callback_thread_id);
	vdo_drain_block_map(completion->vdo->block_map, VDO_ADMIN_STATE_RECOVERING, completion);
}

static bool fetch_page(struct rebuild_completion *rebuild, struct vdo_completion *completion);

/**
 * handle_page_load_error() - Handle an error loading a page.
 * @completion: The vdo_page_completion.
 */
static void handle_page_load_error(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = completion->parent;

	rebuild->outstanding--;
	vdo_set_completion_result(&rebuild->completion, completion->result);
	vdo_release_page_completion(completion);
	fetch_page(rebuild, completion);
}

/**
 * Unmap an invalid entry and indicate that its page must be written out.
 * @page: The page containing the entries
 * @completion: The page_completion for writing the page
 * @slot: The slot to unmap
 */
static void
unmap_entry(struct block_map_page *page, struct vdo_completion *completion, slot_number_t slot)
{
	page->entries[slot] = vdo_pack_block_map_entry(VDO_ZERO_BLOCK, VDO_MAPPING_STATE_UNMAPPED);
	vdo_request_page_write(completion);
}

/**
 * remove_out_of_bounds_entries(): Unmap entries which outside the logical space.
 * @page: The page containing the entries
 * @completion: The page_completion for writing the page
 * @start: The first slot to check
 */
static void remove_out_of_bounds_entries(struct block_map_page *page,
					 struct vdo_completion *completion,
					 slot_number_t start)
{
	slot_number_t slot;

	for (slot = start; slot < VDO_BLOCK_MAP_ENTRIES_PER_PAGE; slot++) {
		struct data_location mapping = vdo_unpack_block_map_entry(&page->entries[slot]);

		if (vdo_is_mapped_location(&mapping))
			unmap_entry(page, completion, slot);
	}
}

/**
 * process_slot(): Update the reference counts for a single entry.
 * @page: The page containing the entries
 * @completion: The page_completion for writing the page
 * @slot: The slot to check
 *
 * Return: true if the entry was a valid mapping
 */
static bool
process_slot(struct block_map_page *page, struct vdo_completion *completion, slot_number_t slot)
{
	struct slab_depot *depot = completion->vdo->depot;
	struct vdo_slab *slab;
	int result;
	struct data_location mapping = vdo_unpack_block_map_entry(&page->entries[slot]);

	if (!vdo_is_valid_location(&mapping)) {
		/* This entry is invalid, so remove it from the page. */
		unmap_entry(page, completion, slot);
		return false;
	}

	if (!vdo_is_mapped_location(&mapping))
		return false;


	if (mapping.pbn == VDO_ZERO_BLOCK)
		return true;

	if (!vdo_is_physical_data_block(depot, mapping.pbn)) {
		/*
		 * This is a nonsense mapping. Remove it from the map so we're at least consistent
		 * and mark the page dirty.
		 */
		unmap_entry(page, completion, slot);
		return false;
	}

	slab = vdo_get_slab(depot, mapping.pbn);
	result = vdo_adjust_reference_count_for_rebuild(slab->reference_counts,
							mapping.pbn,
							VDO_JOURNAL_DATA_REMAPPING);
	if (result == VDO_SUCCESS)
		return true;

	uds_log_error_strerror(result,
			       "Could not adjust reference count for PBN %llu, slot %u mapped to PBN %llu",
			       (unsigned long long) vdo_get_block_map_page_pbn(page),
			       slot,
			       (unsigned long long) mapping.pbn);
	unmap_entry(page, completion, slot);
	return false;
}

/**
 * rebuild_reference_counts_from_page() - Rebuild reference counts from a block map page.
 * @rebuild: The rebuild completion.
 * @completion: The page completion holding the page.
 */
static void rebuild_reference_counts_from_page(struct rebuild_completion *rebuild,
					       struct vdo_completion *completion)
{
	slot_number_t slot, last_slot;
	struct block_map_page *page;
	int result;

	result = vdo_get_cached_page(completion, &page);
	if (result != VDO_SUCCESS) {
		vdo_set_completion_result(&rebuild->completion, result);
		return;
	}

	if (!page->header.initialized)
		return;

	/* Remove any bogus entries which exist beyond the end of the logical space. */
	if (vdo_get_block_map_page_pbn(page) == rebuild->last_slot.pbn) {
		last_slot = rebuild->last_slot.slot;
		remove_out_of_bounds_entries(page, completion, last_slot);
	} else {
		last_slot = VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
	}

	/* Inform the slab depot of all entries on this page. */
	for (slot = 0; slot < last_slot; slot++) {
		if (process_slot(page, completion, slot))
			rebuild->logical_blocks_used++;
	}
}

/**
 * page_loaded() - Process a page which has just been loaded.
 * @completion: The vdo_page_completion for the fetched page.
 *
 * This callback is registered by fetch_page().
 */
static void page_loaded(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = completion->parent;

	rebuild->outstanding--;
	rebuild_reference_counts_from_page(rebuild, completion);
	vdo_release_page_completion(completion);

	/* Advance progress to the next page, and fetch the next page we haven't yet requested. */
	fetch_page(rebuild, completion);
}

static physical_block_number_t
get_pbn_to_fetch(struct rebuild_completion *rebuild, struct block_map *block_map)
{
	physical_block_number_t pbn = VDO_ZERO_BLOCK;

	if (rebuild->completion.result != VDO_SUCCESS)
		return VDO_ZERO_BLOCK;

	while ((pbn == VDO_ZERO_BLOCK) && (rebuild->page_to_fetch < rebuild->leaf_pages))
		pbn = vdo_find_block_map_page_pbn(block_map, rebuild->page_to_fetch++);

	if (vdo_is_physical_data_block(rebuild->completion.vdo->depot, pbn))
		return pbn;

	vdo_set_completion_result(&rebuild->completion, VDO_BAD_MAPPING);
	return VDO_ZERO_BLOCK;
}

/**
 * fetch_page() - Fetch a page from the block map.
 * @rebuild: The rebuild_completion.
 * @completion: The page completion to use.
 *
 * Return true if the rebuild is complete
 */
static bool fetch_page(struct rebuild_completion *rebuild, struct vdo_completion *completion)
{
	struct vdo_page_completion *page_completion = (struct vdo_page_completion *) completion;
	struct block_map *block_map = rebuild->completion.vdo->block_map;
	physical_block_number_t pbn = get_pbn_to_fetch(rebuild, block_map);

	if (pbn != VDO_ZERO_BLOCK) {
		rebuild->outstanding++;
		/*
		 * We must set the requeue flag here to ensure that we don't blow the stack if all
		 * the requested pages are already in the cache or get load errors.
		 */
		vdo_get_page(page_completion,
			     &block_map->zones[0],
			     pbn,
			     true,
			     rebuild,
			     page_loaded,
			     handle_page_load_error,
			     true);
	}

	if (rebuild->outstanding > 0)
		return false;

	vdo_launch_completion_callback(&rebuild->completion,
				       flush_block_map_updates,
				       rebuild->admin_thread_id);
	return true;
}

/**
 * rebuild_from_leaves() - Rebuild reference counts from the leaf block map pages.
 * @completion: The rebuild completion.
 *
 * Rebuilds reference counts from the leaf block map pages now that reference counts have been
 * rebuilt from the interior tree pages (which have been loaded in the process). This callback is
 * registered in rebuild_reference_counts().
 */
static void rebuild_from_leaves(struct vdo_completion *completion)
{
	page_count_t i;
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct block_map *map = completion->vdo->block_map;

	rebuild->logical_blocks_used = 0;

	/*
	 * The PBN calculation doesn't work until the tree pages have been loaded, so we can't set
	 * this value at the start of rebuild.
	 */
	rebuild->last_slot = (struct block_map_slot) {
		.slot = map->entry_count % VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		.pbn = vdo_find_block_map_page_pbn(map, rebuild->leaf_pages - 1),
	};
	if (rebuild->last_slot.slot == 0)
		rebuild->last_slot.slot = VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	for (i = 0; i < rebuild->page_count; i++) {
		if (fetch_page(rebuild, &rebuild->page_completions[i].completion))
			/*
			 * The rebuild has already moved on, so it isn't safe nor is there a need
			 * to launch any more fetches.
			 */
			return;
	}
}

/**
 * process_entry() - Process a single entry from the block map tree.
 * @pbn: A pbn which holds a block map tree page.
 * @completion: The parent completion of the traversal.
 *
 * Implements vdo_entry_callback.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int process_entry(physical_block_number_t pbn, struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct slab_depot *depot = completion->vdo->depot;
	struct vdo_slab *slab;
	int result;

	if ((pbn == VDO_ZERO_BLOCK) || !vdo_is_physical_data_block(depot, pbn))
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "PBN %llu out of range",
					      (unsigned long long) pbn);

	slab = vdo_get_slab(depot, pbn);
	result = vdo_adjust_reference_count_for_rebuild(slab->reference_counts,
							pbn,
							VDO_JOURNAL_BLOCK_MAP_REMAPPING);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "Could not adjust reference count for block map tree PBN %llu",
					      (unsigned long long) pbn);

	rebuild->block_map_data_blocks++;
	return VDO_SUCCESS;
}

/**
 * rebuild_reference_counts() - Rebuild the reference counts from the block map now that all
 *                              journal entries have been applied to the block map.
 * @completion: The rebuild completion.
 *
 * This callback is registered in apply_journal_entries().
 */
static void rebuild_reference_counts(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct vdo *vdo = completion->vdo;
	struct vdo_page_cache *cache = &vdo->block_map->zones[0].page_cache;

	/* We must allocate ref_counts before we can rebuild them. */
	if (abort_rebuild_on_error(vdo_allocate_slab_ref_counts(vdo->depot), rebuild))
		return;

	/*
	 * Completion chaining from page cache hits can lead to stack overflow during the rebuild,
	 * so clear out the cache before this rebuild phase.
	 */
	if (abort_rebuild_on_error(vdo_invalidate_page_cache(cache), rebuild))
		return;

	prepare_rebuild_completion(rebuild, rebuild_from_leaves, rebuild->logical_thread_id);
	vdo_traverse_forest(vdo->block_map, process_entry, completion);
}

/**
 * unpack_entry(): Unpack a recovery journal entry in either format.
 * @vdo: The vdo.
 * @packed: The entry to unpack.
 * @format: The expected format of the entry.
 * @entry: The unpacked entry.
 *
 * Return: true if the entry should be applied.3
 */
static bool unpack_entry(struct vdo *vdo,
			 char *packed,
			 enum vdo_metadata_type format,
			 struct recovery_journal_entry *entry)
{
	if (format == VDO_METADATA_RECOVERY_JOURNAL_2) {
		struct packed_recovery_journal_entry *packed_entry =
			(struct packed_recovery_journal_entry *) packed;

		*entry = vdo_unpack_recovery_journal_entry(packed_entry);
	} else {
		physical_block_number_t low32, high4;

		struct packed_recovery_journal_entry_1 *packed_entry =
			(struct packed_recovery_journal_entry_1 *) packed;

		if (packed_entry->operation == VDO_JOURNAL_DATA_INCREMENT)
			entry->operation = VDO_JOURNAL_DATA_REMAPPING;
		else if (packed_entry->operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT)
			entry->operation = VDO_JOURNAL_BLOCK_MAP_REMAPPING;
		else
			return false;

		low32 = __le32_to_cpu(packed_entry->pbn_low_word);
		high4 = packed_entry->pbn_high_nibble;
		entry->slot = (struct block_map_slot) {
			.pbn = ((high4 << 32) | low32),
			.slot = (packed_entry->slot_low | (packed_entry->slot_high << 6)),
		};
		entry->mapping = vdo_unpack_block_map_entry(&packed_entry->block_map_entry);
		entry->unmapping = (struct data_location) {
			.pbn = VDO_ZERO_BLOCK,
			.state = VDO_MAPPING_STATE_UNMAPPED,
		};
	}

	return (validate_recovery_journal_entry(vdo, entry) == VDO_SUCCESS);
}

/**
 * append_sector_entries() - Append an array of recovery journal entries from a journal block
 *                           sector to the array of numbered mappings in the rebuild completion,
 *                           numbering each entry in the order they are appended.
 * @rebuild: The journal rebuild completion.
 * @entries: The entries in the sector.
 * @format: The format of the sector.
 * @entry_count: The number of entries to append.
 */
static void append_sector_entries(struct rebuild_completion *rebuild,
				  char *entries,
				  enum vdo_metadata_type format,
				  journal_entry_count_t entry_count)
{
	journal_entry_count_t i;
	struct vdo *vdo = rebuild->completion.vdo;
	off_t increment = ((format == VDO_METADATA_RECOVERY_JOURNAL_2)
			   ? sizeof(struct packed_recovery_journal_entry)
			   : sizeof(struct packed_recovery_journal_entry_1));

	for (i = 0; i < entry_count; i++, entries += increment) {
		struct recovery_journal_entry entry;

		if (!unpack_entry(vdo, entries, format, &entry))
			/* When recovering from read-only mode, ignore damaged entries. */
			continue;

		rebuild->entries[rebuild->entry_count] =
			(struct numbered_block_mapping) {
			.block_map_slot = entry.slot,
			.block_map_entry = vdo_pack_block_map_entry(entry.mapping.pbn,
								    entry.mapping.state),
			.number = rebuild->entry_count,
		};
		rebuild->entry_count++;
	}
}

static journal_entry_count_t entries_per_sector(enum vdo_metadata_type format, u8 sector_number)
{
	if (format == VDO_METADATA_RECOVERY_JOURNAL_2)
		return RECOVERY_JOURNAL_ENTRIES_PER_SECTOR;

	return ((sector_number == (VDO_SECTORS_PER_BLOCK - 1))
		? RECOVERY_JOURNAL_1_ENTRIES_IN_LAST_SECTOR
		: RECOVERY_JOURNAL_1_ENTRIES_PER_SECTOR);
}

static void extract_entries_from_block(struct rebuild_completion *rebuild,
				       struct recovery_journal *journal,
				       sequence_number_t sequence,
				       enum vdo_metadata_type format,
				       journal_entry_count_t entries)
{
	sector_count_t i;
	struct recovery_block_header header =
		get_recovery_journal_block_header(journal, rebuild->journal_data, sequence);

	if (!is_exact_recovery_journal_block(journal, &header, sequence, format))
		/* This block is invalid, so skip it. */
		return;

	entries = min(entries, header.entry_count);
	for (i = 1; i < VDO_SECTORS_PER_BLOCK; i++) {
		struct packed_journal_sector *sector =
			get_sector(journal, rebuild->journal_data, sequence, i);
		journal_entry_count_t sector_entries = min(entries, entries_per_sector(format, i));

		if (vdo_is_valid_recovery_journal_sector(&header, sector, i)) {
			/* Only extract as many as the block header calls for. */
			append_sector_entries(rebuild,
					      (char *) sector->entries,
					      format,
					      min_t(journal_entry_count_t,
						    sector->entry_count,
						    sector_entries));
		}

		/*
		 * Even if the sector wasn't full, count it as full when counting up to the
		 * entry count the block header claims.
		 */
		entries -= sector_entries;
	}
}

/**
 * extract_journal_entries() - Create an array of all valid journal entries, in order, and store it
 *                             in the rebuild completion.
 * @rebuild: The journal rebuild completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int extract_journal_entries(struct rebuild_completion *rebuild)
{
	int result;
	sequence_number_t i;
	block_count_t count;
	enum vdo_metadata_type format;
	struct vdo *vdo = rebuild->completion.vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	journal_entry_count_t entries_per_block = journal->entries_per_block;

	if (!find_recovery_journal_head_and_tail(journal,
						 rebuild->journal_data,
						 &rebuild->tail,
						 &rebuild->head,
						 NULL))
		return VDO_SUCCESS;

	format = get_recovery_journal_block_header(journal,
						   rebuild->journal_data,
						   rebuild->tail).metadata_type;
	if (format == VDO_METADATA_RECOVERY_JOURNAL)
		entries_per_block = RECOVERY_JOURNAL_1_ENTRIES_PER_BLOCK;

	/*
	 * Allocate an array of numbered_block_mapping structures large enough to transcribe every
	 * packed_recovery_journal_entry from every valid journal block.
	 */
	count = ((rebuild->tail - rebuild->head + 1) * entries_per_block);
	result = UDS_ALLOCATE(count, struct numbered_block_mapping, __func__, &rebuild->entries);
	if (result != VDO_SUCCESS)
		return result;

	for (i = rebuild->head; i <= rebuild->tail; i++)
		extract_entries_from_block(rebuild, journal, i, format, entries_per_block);

	return VDO_SUCCESS;
}

/**
 * apply_journal_entries() - Determine the limits of the valid recovery journal and apply all valid
 *                           entries to the block map.
 * @completion: The rebuild completion.
 *
 * This callback is registered in load_journal_callback().
 */
static void apply_journal_entries(struct vdo_completion *completion)
{
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);


	if (abort_rebuild_on_error(extract_journal_entries(rebuild), rebuild))
		return;

	/* Suppress block map errors. */
	vdo->block_map->zones[0].page_cache.rebuilding = true;

	/* Play the recovery journal into the block map. */
	prepare_rebuild_completion(rebuild,
				   rebuild_reference_counts,
				   completion->callback_thread_id);
	recover_block_map(vdo, rebuild->entry_count, rebuild->entries, completion);
}

/**
 * launch_rebuild() - Construct a rebuild_completion and launch it.
 * @parent: The completion to notify when the rebuild is complete.
 * @journal_data: The contents of the recovery journal
 *
 * Apply all valid journal block entries to all vdo structures.
 */
static void launch_rebuild(struct vdo_completion *parent, char *journal_data)
{
	struct vdo *vdo = parent->vdo;
	struct rebuild_completion *rebuild;
	page_count_t page_count;
	int result;

	page_count = min_t(page_count_t,
			   vdo->device_config->cache_size >> 1,
			   MAXIMUM_SIMULTANEOUS_VDO_BLOCK_MAP_RESTORATION_READS);
	result = UDS_ALLOCATE_EXTENDED(struct rebuild_completion,
				       page_count,
				       struct vdo_page_completion,
				       __func__,
				       &rebuild);
	if (result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		vdo_fail_completion(parent, result);
		return;
	}

	vdo_initialize_completion(&rebuild->completion, vdo, VDO_READ_ONLY_REBUILD_COMPLETION);
	rebuild->completion.parent = parent;
	rebuild->completion.error_handler = abort_rebuild;
	rebuild->page_count = page_count;
	rebuild->leaf_pages = vdo_compute_block_map_page_count(vdo->block_map->entry_count);
	rebuild->logical_thread_id = vdo_get_logical_zone_thread(vdo->thread_config, 0);
	rebuild->admin_thread_id = vdo->thread_config->admin_thread;
	rebuild->journal_data = journal_data;

	prepare_rebuild_completion(rebuild, apply_journal_entries, rebuild->logical_thread_id);
	vdo_load_slab_depot(vdo->depot,
			    VDO_ADMIN_STATE_LOADING_FOR_REBUILD,
			    &rebuild->completion,
			    NULL);
}

static void free_journal_loader(struct journal_loader *loader)
{
	if (loader == NULL)
		return;

	while (loader->count-- > 0)
		free_vio(UDS_FORGET(loader->vios[loader->count]));

	UDS_FREE(loader);
}

/**
 * finish_journal_load() - Handle the completion of a journal read, and if it is the last one,
 *                         finish the load by notifying the parent.
 */
static void finish_journal_load(struct vdo_completion *completion)
{
	struct journal_loader *loader = completion->parent;
	struct vdo_completion *parent = loader->parent;
	struct vdo *vdo = parent->vdo;
	char *journal_data = loader->journal_data;

	if (++loader->complete != loader->count)
		return;

	uds_log_info("Finished reading recovery journal");
	free_journal_loader(UDS_FORGET(loader));
	if (parent->result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		vdo_finish_completion(parent);
		return;
	}

	if (vdo_state_requires_recovery(vdo->load_state))
		launch_recovery(parent, journal_data);
	else
		launch_rebuild(parent, journal_data);
}

static void handle_journal_load_error(struct vdo_completion *completion)
{
	struct journal_loader *loader = completion->parent;

	/* Preserve the error */
	vdo_set_completion_result(loader->parent, completion->result);
	record_metadata_io_error(as_vio(completion));
	completion->callback(completion);
}

static void read_journal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vdo *vdo = vio->completion.vdo;

	continue_vio_after_io(vio, finish_journal_load, vdo->thread_config->admin_thread);
}

/**
 * vdo_repair(): Load the recovery journal and then recover or rebuild a vdo.
 * @parent: The completion to notify when the operation is complete
 */
void vdo_repair(struct vdo_completion *parent)
{
	int result;
	char *ptr;
	struct journal_loader *loader;
	struct vdo *vdo = parent->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	physical_block_number_t pbn = journal->origin;
	block_count_t remaining = journal->size;
	block_count_t vio_count = DIV_ROUND_UP(remaining, MAX_BLOCKS_PER_VIO);

	vdo_assert_on_admin_thread(vdo, __func__);

#ifdef VDO_INTERNAL
	/* These messages must be in sync with Permabit::VDODeviceBase. */
#endif /* VDO_INTERNAL */
	if (vdo->load_state == VDO_FORCE_REBUILD) {
		uds_log_warning("Rebuilding reference counts to clear read-only mode");
		vdo->states.vdo.read_only_recoveries++;
	} else if (vdo->load_state == VDO_REBUILD_FOR_UPGRADE) {
		uds_log_warning("Rebuilding reference counts for upgrade");
	} else {
		uds_log_warning("Device was dirty, rebuilding reference counts");
	}

	result = UDS_ALLOCATE(remaining * VDO_BLOCK_SIZE, char, __func__, &ptr);
	if (result != VDO_SUCCESS) {
		vdo_fail_completion(parent, result);
		return;
	}

	result = UDS_ALLOCATE_EXTENDED(struct journal_loader,
				       vio_count,
				       struct vio *,
				       __func__,
				       &loader);
	if (result != VDO_SUCCESS) {
		UDS_FREE(ptr);
		vdo_fail_completion(parent, result);
		return;
	}

	loader->parent = parent;
	loader->journal_data = ptr;
	for (loader->count = 0; loader->count < vio_count; loader->count++) {
		block_count_t blocks = min_t(block_count_t, remaining, MAX_BLOCKS_PER_VIO);

		result = create_multi_block_metadata_vio(parent->vdo,
							 VIO_TYPE_RECOVERY_JOURNAL,
							 VIO_PRIORITY_METADATA,
							 loader,
							 blocks,
							 ptr,
							 &loader->vios[loader->count]);
		if (result != VDO_SUCCESS) {
			free_journal_loader(UDS_FORGET(loader));
			UDS_FREE(ptr);
			vdo_fail_completion(parent, result);
			return;
		}

		ptr += (blocks * VDO_BLOCK_SIZE);
		remaining -= blocks;
	}

	for (vio_count = 0; vio_count < loader->count; vio_count++, pbn += MAX_BLOCKS_PER_VIO)
		submit_metadata_vio(loader->vios[vio_count],
				    pbn,
				    read_journal_endio,
				    handle_journal_load_error,
				    REQ_OP_READ);
}
