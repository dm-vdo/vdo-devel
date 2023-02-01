// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-recovery.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-allocator.h"
#include "block-map-recovery.h"
#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "forest.h"
#include "int-map.h"
#include "io-submitter.h"
#include "journal-point.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab.h"
#include "thread-config.h"
#include "types.h"
#include "vdo-component-states.h"
#include "vdo-component.h"
#include "vdo-page-cache.h"
#include "vdo.h"
#include "wait-queue.h"

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
};

struct recovery_completion {
	/* The completion header */
	struct vdo_completion completion;
	/* A buffer to hold the data read off disk */
	char *journal_data;
	/* The number of increfs */
	size_t incref_count;

	/* The entry data for the block map recovery */
	struct numbered_block_mapping *entries;
	/* The number of entries in the entry array */
	size_t entry_count;
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

	/* Decref synthesis fields */

	/* An int_map for use in finding which slots are missing decrefs */
	struct int_map *slot_entry_map;
	/* The number of synthesized decrefs */
	size_t missing_decref_count;
	/* The number of incomplete decrefs */
	size_t incomplete_decref_count;
	/* The fake journal point of the next missing decref */
	struct journal_point next_synthesized_journal_point;
	/* The queue of missing decrefs */
	struct wait_queue missing_decrefs[];
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

enum {
	/* The int map needs capacity of twice the number of VIOs in the system. */
	INT_MAP_CAPACITY = MAXIMUM_VDO_USER_VIOS * 2,
	/* There can be as many missing decrefs as there are VIOs in the system. */
	MAXIMUM_SYNTHESIZED_DECREFS = MAXIMUM_VDO_USER_VIOS,
};

struct missing_decref {
	/* A waiter for queueing this object */
	struct waiter waiter;
	/* The parent of this object */
	struct recovery_completion *recovery;
	/* Whether this decref is complete */
	bool complete;
	/* The slot for which the last decref was lost */
	struct block_map_slot slot;
	/* The penultimate block map entry for this LBN */
	struct data_location penultimate_mapping;
	/* The page completion used to fetch the block map page for this LBN */
	struct vdo_page_completion page_completion;
	/* The journal point which will be used for this entry */
	struct journal_point journal_point;
	/* The slab journal to which this entry will be applied */
	struct slab_journal *slab_journal;
};

/**
 * as_missing_decref() - Convert a waiter to the missing decref of which it is a part.
 * @waiter: The waiter to convert.
 *
 * Return: The missing_decref wrapping the waiter.
 */
static inline struct missing_decref * __must_check as_missing_decref(struct waiter *waiter)
{
	return container_of(waiter, struct missing_decref, waiter);
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
	vdo_assert_completion_type(completion->type, VDO_RECOVERY_COMPLETION);
	return container_of(completion, struct recovery_completion, completion);
}

/**
 * slot_as_number() - Convert a block_map_slot into a unique u64.
 * @slot: The block map slot to convert.
 *
 * Return: A one-to-one mappable u64.
 */
static u64 slot_as_number(struct block_map_slot slot)
{
	return (((u64) slot.pbn << 10) + slot.slot);
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
 *
 * A block is not valid if it is unformatted, or if it is older than the last successful recovery
 * or reformat.
 *
 * Return: True if the header is valid.
 */
static inline bool __must_check
is_valid_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header)
{
	return ((header->metadata_type == VDO_METADATA_RECOVERY_JOURNAL) &&
		(header->nonce == journal->nonce) &&
		(header->recovery_count == journal->recovery_count));
}

/**
 * is_exact_recovery_journal_block() - Determine whether the given header describes the exact block
 *                                     indicated.
 * @journal: The journal to use.
 * @header: The unpacked block header to check.
 * @sequence: The expected sequence number.
 *
 * Return: True if the block matches.
 */
static inline bool __must_check
is_exact_recovery_journal_block(const struct recovery_journal *journal,
				const struct recovery_block_header *header,
				sequence_number_t sequence)
{
	return ((header->sequence_number == sequence) &&
		is_valid_recovery_journal_block(journal, header));
}

/**
 * is_congruent_recovery_journal_block() - Determine whether the given header describes a valid
 *                                         block for the given journal that could appear at the
 *                                         given offset in the journal.
 * @journal: The journal to use.
 * @header: The unpacked block header to check.
 * @offset: An offset indicating where the block was in the journal.
 *
 * Return: True if the header matches.
 */
static bool __must_check
is_congruent_recovery_journal_block(struct recovery_journal *journal,
				    const struct recovery_block_header *header,
				    physical_block_number_t offset)
{
	physical_block_number_t expected_offset =
		vdo_get_recovery_journal_block_number(journal, header->sequence_number);

	return ((expected_offset == offset) && is_valid_recovery_journal_block(journal, header));
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
 * Return: True if there were valid journal blocks
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

		if (!is_congruent_recovery_journal_block(journal, &header, i))
			/* This block is old, unformatted, or doesn't belong at this location. */
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
 * make_missing_decref() - Create a missing_decref and enqueue it to wait for a determination of
 *                         its penultimate mapping.
 * @recovery: The parent recovery completion.
 * @entry: The recovery journal entry for the increment which is missing a decref.
 * @decref_ptr: A pointer to hold the new missing_decref.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check
make_missing_decref(struct recovery_completion *recovery,
		    struct recovery_journal_entry entry,
		    struct missing_decref **decref_ptr)
{
	struct missing_decref *decref;
	int result;

	result = UDS_ALLOCATE(1, struct missing_decref, __func__, &decref);
	if (result != VDO_SUCCESS)
		return result;

	decref->recovery = recovery;
	enqueue_waiter(&recovery->missing_decrefs[0], &decref->waiter);

	/*
	 * Each synthsized decref needs a unique journal point. Otherwise, in the event of a crash,
	 * we would be unable to tell which synthesized decrefs had already been committed in the
	 * slab journals. Instead of using real recovery journal space for this, we can use fake
	 * journal points between the last currently valid entry in the tail block and the first
	 * journal entry in the next block. We can't overflow the entry count since the number of
	 * synthesized decrefs is bounded by the data VIO limit.
	 *
	 * It is vital that any given missing decref always have the same fake journal point since
	 * a failed recovery may be retried with a different number of zones after having written
	 * out some slab journal blocks. Since the missing decrefs are always read out of the
	 * journal in the same order, we can assign them a journal point when they are read. Their
	 * subsequent use will ensure that, for any given slab journal, they are applied in the
	 * order dictated by these assigned journal points.
	 */
	decref->slot = entry.slot;
	decref->journal_point = recovery->next_synthesized_journal_point;
	recovery->next_synthesized_journal_point.entry_count++;
	recovery->missing_decref_count++;
	recovery->incomplete_decref_count++;

	*decref_ptr = decref;
	return VDO_SUCCESS;
}

/**
 * increment_recovery_point() - Move the given recovery point forward by one entry.
 * @point: The recovery point to alter.
 */
static void increment_recovery_point(struct recovery_point *point)
{
	point->entry_count++;
	if ((point->sector_count == (VDO_SECTORS_PER_BLOCK - 1)) &&
	    (point->entry_count == RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR)) {
		point->sequence_number++;
		point->sector_count = 1;
		point->entry_count = 0;
	}

	if (point->entry_count == RECOVERY_JOURNAL_ENTRIES_PER_SECTOR) {
		point->sector_count++;
		point->entry_count = 0;
		return;
	}
}

/**
 * decrement_recovery_point() - Move the given recovery point backwards by one entry.
 * @point: The recovery point to alter.
 */
static void decrement_recovery_point(struct recovery_point *point)
{
	STATIC_ASSERT(RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR > 0);

	if ((point->sector_count <= 1) && (point->entry_count == 0)) {
		point->sequence_number--;
		point->sector_count = VDO_SECTORS_PER_BLOCK - 1;
		point->entry_count = RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR - 1;
		return;
	}

	if (point->entry_count == 0) {
		point->sector_count--;
		point->entry_count = RECOVERY_JOURNAL_ENTRIES_PER_SECTOR - 1;
		return;
	}

	point->entry_count--;
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
 * free_missing_decref() - A waiter callback to free missing_decrefs.
 *
 * Implements waiter_callback.
 */
static void free_missing_decref(struct waiter *waiter, void *context __always_unused)
{
	UDS_FREE(as_missing_decref(waiter));
}

/**
 * free_vdo_recovery_completion() - Free a recovery_completion and all underlying structures.
 * @recovery: The recovery completion to free.
 */
static void free_vdo_recovery_completion(struct recovery_completion *recovery)
{
	zone_count_t zone, zone_count;

	if (recovery == NULL)
		return;

	free_int_map(UDS_FORGET(recovery->slot_entry_map));
	zone_count = recovery->completion.vdo->thread_config->physical_zone_count;
	for (zone = 0; zone < zone_count; zone++)
		notify_all_waiters(&recovery->missing_decrefs[zone], free_missing_decref, NULL);

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

	vdo_finish_completion(&recovery->completion, result);
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
	    !vdo_is_physical_data_block(vdo->depot, entry->mapping.pbn))
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: (%llu, %u) to %llu (%s) is not within bounds",
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->mapping.pbn,
					      vdo_get_journal_operation_name(entry->operation));

	if ((entry->operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) &&
	    (vdo_is_state_compressed(entry->mapping.state) ||
	    (entry->mapping.pbn == VDO_ZERO_BLOCK)))
		return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
					      "Invalid entry: (%llu, %u) to %llu (%s) is not a valid tree mapping",
					      (unsigned long long) entry->slot.pbn,
					      entry->slot.slot,
					      (unsigned long long) entry->mapping.pbn,
					      vdo_get_journal_operation_name(entry->operation));

	return VDO_SUCCESS;
}

/**
 * extract_increment() - Create an array of all valid increment entries, in order, and store it in
 *                       the recovery completion.
 * @recovery: The recovery completion.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int extract_increment_entries(struct recovery_completion *recovery)
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
	 * every increment packed_recovery_journal_entry from every valid journal block.
	 */
	result = UDS_ALLOCATE(recovery->incref_count,
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
			vdo_enter_read_only_mode(vdo->read_only_notifier, result);
			return result;
		}

		if (!vdo_is_journal_increment_operation(entry.operation))
			continue;

		recovery->entries[recovery->entry_count] = (struct numbered_block_mapping) {
			.block_map_slot = entry.slot,
			.block_map_entry = vdo_pack_block_map_entry(entry.mapping.pbn, entry.mapping.state),
			.number = recovery->entry_count,
		};
		recovery->entry_count++;
	}

	result = ASSERT((recovery->entry_count <= recovery->incref_count),
			"approximate incref count is an upper bound");
	if (result != VDO_SUCCESS)
		vdo_enter_read_only_mode(vdo->read_only_notifier, result);

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
	result = extract_increment_entries(recovery);
	if (abort_recovery_on_error(result, recovery))
		return;

	prepare_recovery_completion(recovery, finish_recovery, VDO_ZONE_TYPE_ADMIN);
	vdo_recover_block_map(vdo, recovery->entry_count, recovery->entries, completion);
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
	uds_log_info("Synthesized %zu missing journal entries", recovery->missing_decref_count);

	vdo->recovery_journal->logical_blocks_used = recovery->logical_blocks_used;
	vdo->recovery_journal->block_map_data_blocks = recovery->block_map_data_blocks;

	prepare_recovery_completion(recovery, start_super_block_save, VDO_ZONE_TYPE_ADMIN);
	vdo_drain_slab_depot(vdo->depot, VDO_ADMIN_STATE_RECOVERING, completion);
}

/**
 * add_synthesized_entries() - Add synthesized entries into slab journals, waiting when necessary.
 * @completion: The allocator completion.
 */
static void add_synthesized_entries(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);
	struct recovery_completion *recovery = completion->parent;
	struct wait_queue *missing_decrefs = &recovery->missing_decrefs[allocator->zone_number];

	/* Get ready in case we need to enqueue again */
	vdo_prepare_completion(completion,
			       add_synthesized_entries,
			       vdo_notify_slab_journals_are_recovered,
			       completion->callback_thread_id,
			       recovery);
	while (has_waiters(missing_decrefs)) {
		struct missing_decref *decref =
			as_missing_decref(get_first_waiter(missing_decrefs));

		if (!vdo_attempt_replay_into_slab_journal(decref->slab_journal,
							  decref->penultimate_mapping.pbn,
							  VDO_JOURNAL_DATA_DECREMENT,
							  &decref->journal_point,
							  completion))
			return;

		dequeue_next_waiter(missing_decrefs);
		UDS_FREE(decref);
	}

	vdo_notify_slab_journals_are_recovered(completion);
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
	struct recovery_journal *journal = recovery->completion.vdo->recovery_journal;
	struct recovery_block_header header =
		get_recovery_journal_block_header(journal, recovery->journal_data, recovery->tail);

	recovery->logical_blocks_used = header.logical_blocks_used;
	recovery->block_map_data_blocks = header.block_map_data_blocks;

	for (; before_recovery_point(&recovery_point, &recovery->tail_recovery_point);
	     increment_recovery_point(&recovery_point)) {
		struct recovery_journal_entry entry = get_entry(recovery, &recovery_point);

		if (!vdo_is_mapped_location(&entry.mapping))
			continue;

		switch (entry.operation) {
		case VDO_JOURNAL_DATA_INCREMENT:
			recovery->logical_blocks_used++;
			break;

		case VDO_JOURNAL_DATA_DECREMENT:
			recovery->logical_blocks_used--;
			break;

		case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
			recovery->block_map_data_blocks++;
			break;

		default:
			return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						      "Recovery journal entry at sequence number %llu, sector %u, entry %u had invalid operation %u",
						      (unsigned long long) recovery_point.sequence_number,
						      recovery_point.sector_count,
						      recovery_point.entry_count,
						      entry.operation);
		}
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
	increment_recovery_point(&recovery->next_recovery_point);
	vdo_advance_journal_point(&recovery->next_journal_point, entries_per_block);
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
		struct vdo_slab *slab;
		struct recovery_journal_entry entry = get_entry(recovery, recovery_point);
		int result;

		result = validate_recovery_journal_entry(vdo, &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(journal->read_only_notifier, result);
			vdo_finish_completion(completion, result);
			return;
		}

		if (entry.mapping.pbn == VDO_ZERO_BLOCK)
			continue;

		slab = vdo_get_slab(vdo->depot, entry.mapping.pbn);
		if (slab->allocator != allocator)
			continue;

		if (!vdo_attempt_replay_into_slab_journal(slab->journal,
							  entry.mapping.pbn,
							  entry.operation,
							  &recovery->next_journal_point,
							  completion))
			return;

		recovery->entries_added_to_slab_journals++;
	}

	uds_log_info("Recreating missing journal entries for zone %u", allocator->zone_number);
	add_synthesized_entries(completion);
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

/**
 * queue_on_physical_zone() - A waiter callback to enqueue a missing_decref on the queue for the
 *                            physical zone in which it will be applied.
 *
 * Implements waiter_callback.
 */
static void queue_on_physical_zone(struct waiter *waiter, void *context)
{
	zone_count_t zone_number;
	struct missing_decref *decref = as_missing_decref(waiter);
	struct data_location mapping = decref->penultimate_mapping;

	if (vdo_is_mapped_location(&mapping))
		decref->recovery->logical_blocks_used--;

	if (mapping.pbn == VDO_ZERO_BLOCK) {
		/* Decrefs of zero are not applied to slab journals. */
		UDS_FREE(decref);
		return;
	}

	decref->slab_journal = vdo_get_slab((struct slab_depot *) context, mapping.pbn)->journal;
	zone_number = decref->slab_journal->slab->allocator->zone_number;
	enqueue_waiter(&decref->recovery->missing_decrefs[zone_number], &decref->waiter);
}

/**
 * apply_to_depot() - Queue each missing decref on the slab journal to which it is to be applied
 *                    then load the slab depot.
 * @completion: The recovery completion.
 *
 * This callback is registered in find_slab_journal_entries().
 */
static void apply_to_depot(struct vdo_completion *completion)
{
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;
	struct slab_depot *depot = vdo->depot;

	vdo_assert_on_admin_thread(vdo, __func__);
	prepare_recovery_completion(recovery, finish_recovering_depot, VDO_ZONE_TYPE_ADMIN);
	notify_all_waiters(&recovery->missing_decrefs[0], queue_on_physical_zone, depot);
	if (abort_recovery_on_error(completion->result, recovery))
		return;

	vdo_load_slab_depot(depot, VDO_ADMIN_STATE_LOADING_FOR_RECOVERY, completion, recovery);
}

/**
 * record_missing_decref() - Validate the location of the penultimate mapping for a missing_decref.
 * @decref: The decref whose penultimate mapping has just been found.
 * @location: The penultimate mapping.
 * @error_code: The error code to use if the location is invalid.
 *
 * If it is valid, enqueue it for the appropriate physical zone or account for it. Otherwise,
 * dispose of it and signal an error.
 */
static int
record_missing_decref(struct missing_decref *decref, struct data_location location, int error_code)
{
	struct recovery_completion *recovery = decref->recovery;
	struct vdo *vdo = recovery->completion.vdo;

	recovery->incomplete_decref_count--;
	if (vdo_is_valid_location(&location) &&
	    vdo_is_physical_data_block(vdo->depot, location.pbn)) {
		decref->penultimate_mapping = location;
		decref->complete = true;
		return VDO_SUCCESS;
	}

	/* The location was invalid */
	vdo_enter_read_only_mode(vdo->read_only_notifier, error_code);
	vdo_set_completion_result(&recovery->completion, error_code);
	uds_log_error_strerror(error_code,
			       "Invalid mapping for pbn %llu with state %u",
			       (unsigned long long) location.pbn,
			       location.state);
	return error_code;
}

/**
 * find_missing_decrefs() - Find the block map slots with missing decrefs.
 * @recovery: The recovery completion.
 *
 * To find the slots missing decrefs, we iterate through the journal in reverse so we see decrefs
 * before increfs; if we see an incref before its paired decref, we instantly know this incref is
 * missing its decref.
 *
 * Simultaneously, we attempt to determine the missing decref. If there is a missing decref, and at
 * least two increfs for that slot, we know we should decref the PBN from the penultimate incref.
 * Otherwise, there is only one incref for that slot: we must synthesize the decref out of the
 * block map instead of the recovery journal.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int __must_check find_missing_decrefs(struct recovery_completion *recovery)
{
	/*
	 * This placeholder decref is used to mark lbns for which we have observed a decref but not
	 * the paired incref (going backwards through the journal).
	 */
	struct missing_decref found_decref;
	int result;
	struct recovery_journal_entry entry;
	struct missing_decref *decref;
	struct recovery_point recovery_point;
	struct int_map *slot_entry_map = recovery->slot_entry_map;
	/*
	 * A buffer is allocated based on the number of incref entries found, so use the earliest
	 * head.
	 */
	sequence_number_t head = min(recovery->block_map_head, recovery->slab_journal_head);
	struct recovery_point head_point = {
		.sequence_number = head,
		.sector_count = 1,
		.entry_count = 0,
	};

	/* Set up for the first fake journal point that will be used for a synthesized entry. */
	recovery->next_synthesized_journal_point = (struct journal_point) {
		.sequence_number = recovery->tail,
		.entry_count = recovery->completion.vdo->recovery_journal->entries_per_block,
	};

	recovery_point = recovery->tail_recovery_point;
	while (before_recovery_point(&head_point, &recovery_point)) {
		decrement_recovery_point(&recovery_point);
		entry = get_entry(recovery, &recovery_point);

		if (!vdo_is_journal_increment_operation(entry.operation)) {
			/*
			 * Observe that we've seen a decref before its incref, but only if the
			 * int_map does not contain an unpaired incref for this lbn.
			 */
			result = int_map_put(slot_entry_map,
					     slot_as_number(entry.slot),
					     &found_decref,
					     false,
					     NULL);
			if (result != VDO_SUCCESS)
				return result;

			continue;
		}

		recovery->incref_count++;

		decref = int_map_remove(slot_entry_map, slot_as_number(entry.slot));
		if (entry.operation == VDO_JOURNAL_BLOCK_MAP_INCREMENT) {
			if (decref != NULL)
				return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
							      "decref found for block map block %llu with state %u",
							      (unsigned long long) entry.mapping.pbn,
							      entry.mapping.state);

			/* There are no decrefs for block map pages, so they can't be missing. */
			continue;
		}

		if (decref == &found_decref)
			/*
			 * This incref already had a decref in the intmap, so we know it is not
			 * missing its decref.
			 */
			continue;

		if (decref == NULL) {
			/* This incref is missing a decref. Add a missing decref object. */
			result = make_missing_decref(recovery, entry, &decref);
			if (result != VDO_SUCCESS)
				return result;

			result = int_map_put(slot_entry_map,
					     slot_as_number(entry.slot),
					     decref,
					     false,
					     NULL);
			if (result != VDO_SUCCESS)
				return result;

			continue;
		}

		/*
		 * This missing decref was left here by an incref without a decref. We now know
		 * what its penultimate mapping is, and all entries before here in the journal are
		 * paired, decref before incref, so we needn't remember it in the intmap any
		 * longer.
		 */
		result = record_missing_decref(decref, entry.mapping, VDO_CORRUPT_JOURNAL);
		if (result != VDO_SUCCESS)
			return result;
	}

	return VDO_SUCCESS;
}

/**
 * process_fetched_page() - Process a fetched block map page for a missing decref.
 * @completion: The page completion which has just finished loading.
 *
 * This callback is registered in find_slab_journal_entries().
 */
static void process_fetched_page(struct vdo_completion *completion)
{
	struct missing_decref *current_decref = completion->parent;
	struct recovery_completion *recovery = current_decref->recovery;
	const struct block_map_page *page;
	struct data_location location;

	vdo_assert_on_logical_zone_thread(completion->vdo, 0, __func__);

	page = vdo_dereference_readable_page(completion);
	location = vdo_unpack_block_map_entry(&page->entries[current_decref->slot.slot]);
	vdo_release_page_completion(completion);
	record_missing_decref(current_decref, location, VDO_BAD_MAPPING);
	if (recovery->incomplete_decref_count == 0)
		vdo_invoke_completion_callback(&recovery->completion);
}

/**
 * handle_fetch_error() - Handle an error fetching a block map page for a missing decref.
 * @completion: The page completion which has just finished loading.
 *
 * This error handler is registered in find_slab_journal_entries().
 */
static void handle_fetch_error(struct vdo_completion *completion)
{
	struct missing_decref *decref = completion->parent;
	struct recovery_completion *recovery = decref->recovery;
	int result = completion->result;

	vdo_assert_on_logical_zone_thread(completion->vdo, 0, __func__);

	/*
	 * If we got a VDO_OUT_OF_RANGE error, it is because the pbn we read from the journal was
	 * bad, so convert the error code
	 */
	if (result == VDO_OUT_OF_RANGE)
		result = VDO_CORRUPT_JOURNAL;

	vdo_set_completion_result(&recovery->completion, result);
	vdo_release_page_completion(completion);
	if (--recovery->incomplete_decref_count == 0)
		vdo_invoke_completion_callback(&recovery->completion);
}

/**
 * launch_fetch() - The waiter callback to requeue a missing decref and launch its page fetch.
 *
 * Implements waiter_callback.
 */
static void launch_fetch(struct waiter *waiter, void *context)
{
	struct missing_decref *decref = as_missing_decref(waiter);
	struct recovery_completion *recovery = decref->recovery;
	struct block_map_zone *zone = context;

	enqueue_waiter(&recovery->missing_decrefs[0], &decref->waiter);
	if (decref->complete)
		/* We've already found the mapping for this decref, no fetch needed. */
		return;

	vdo_init_page_completion(&decref->page_completion,
				 zone->page_cache,
				 decref->slot.pbn,
				 false,
				 decref,
				 process_fetched_page,
				 handle_fetch_error);
	vdo_get_page(&decref->page_completion.completion);
}

/**
 * find_slab_journal_entries() - Find all entries which need to be replayed into the slab journals.
 *
 * @completion: The recovery completion.
 */
static void find_slab_journal_entries(struct vdo_completion *completion)
{
	struct recovery_completion *recovery = as_recovery_completion(completion);
	struct vdo *vdo = completion->vdo;

	/* We need to be on logical zone 0's thread since we are going to use its page cache. */
	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	if (abort_recovery_on_error(find_missing_decrefs(recovery), recovery))
		return;

	prepare_recovery_completion(recovery, apply_to_depot, VDO_ZONE_TYPE_ADMIN);

	/*
	 * Increment the incomplete_decref_count so that the fetch callback can't complete while we
	 * are still processing the queue of missing decrefs.
	 */
	if (recovery->incomplete_decref_count++ > 0)
		/* Fetch block map pages to fill in the incomplete missing decrefs. */
		notify_all_waiters(&recovery->missing_decrefs[0],
				   launch_fetch,
				   &vdo->block_map->zones[0]);

	if (--recovery->incomplete_decref_count == 0)
		vdo_complete_completion(completion);
}

/**
 * find_contiguous_range() - Find the contiguous range of journal blocks.
 * @recovery: The recovery completion.
 *
 * Return: true if there were valid journal blocks.
 */
static bool find_contiguous_range(struct recovery_completion *recovery)
{
	struct recovery_journal *journal = recovery->completion.vdo->recovery_journal;
	sequence_number_t head = min(recovery->block_map_head, recovery->slab_journal_head);
	bool found_entries = false;
	sequence_number_t i;

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
		if (!is_exact_recovery_journal_block(journal, &header, i) ||
		    (header.entry_count > journal->entries_per_block))
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
			if (!vdo_is_valid_recovery_journal_sector(&header, sector))
				break;

			if (sector_entries > 0) {
				found_entries = true;
				recovery->tail_recovery_point.sector_count++;
				recovery->tail_recovery_point.entry_count = sector_entries;
				block_entries -= sector_entries;
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

	/* Set the tail to the last valid tail block, if there is one. */
	if (found_entries && (recovery->tail_recovery_point.sector_count == 0))
		recovery->tail--;

	return found_entries;
}

/**
 * count_increment_entries() - Count the number of increment entries in the journal.
 * @recovery: The recovery completion.
 */
static noinline int count_increment_entries(struct recovery_completion *recovery)
{
	/*
	 * XXX VDO-5182: function is declared noinline to avoid what is likely a spurious valgrind
	 * error about this structure being uninitialized.
	 */
	struct recovery_point recovery_point = {
		.sequence_number = recovery->block_map_head,
		.sector_count = 1,
		.entry_count = 0,
	};
	struct vdo *vdo = recovery->completion.vdo;

	while (before_recovery_point(&recovery_point, &recovery->tail_recovery_point)) {
		struct recovery_journal_entry entry = get_entry(recovery, &recovery_point);
		int result;

		result = validate_recovery_journal_entry(vdo, &entry);
		if (result != VDO_SUCCESS) {
			vdo_enter_read_only_mode(vdo->read_only_notifier, result);
			return result;
		}

		if (vdo_is_journal_increment_operation(entry.operation))
			recovery->incref_count++;

		increment_recovery_point(&recovery_point);
	}

	return VDO_SUCCESS;
}

/**
 * prepare_to_apply_journal_entries() - Determine the limits of the valid recovery journal and
 *                                      prepare to replay into the slab journals and block map.
 * @recovery: The recovery completion.
 */
static void prepare_to_apply_journal_entries(struct recovery_completion *recovery)
{
	bool found_entries;
	int result;
	struct vdo_completion *completion = &recovery->completion;
	struct vdo *vdo = completion->vdo;
	struct recovery_journal *journal = vdo->recovery_journal;

	found_entries = find_recovery_journal_head_and_tail(journal,
							    recovery->journal_data,
							    &recovery->highest_tail,
							    &recovery->block_map_head,
							    &recovery->slab_journal_head);
	if (found_entries)
		found_entries = find_contiguous_range(recovery);

	/* Both reap heads must be behind the tail. */
	if ((recovery->block_map_head > recovery->tail) ||
	    (recovery->slab_journal_head > recovery->tail)) {
		result = uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						"Journal tail too early. block map head: %llu, slab journal head: %llu, tail: %llu",
						(unsigned long long) recovery->block_map_head,
						(unsigned long long) recovery->slab_journal_head,
						(unsigned long long) recovery->tail);
		vdo_finish_completion(completion, result);
		return;
	}

	if (!found_entries) {
		/* This message must be in sync with VDOTest::RebuildBase. */
		uds_log_info("Replaying 0 recovery entries into block map");
		/* We still need to load the slab_depot. */
		UDS_FREE(UDS_FORGET(recovery->journal_data));
		prepare_recovery_completion(recovery, finish_recovery, VDO_ZONE_TYPE_ADMIN);
		vdo_load_slab_depot(vdo->depot,
				    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
				    completion,
				    recovery);
		return;
	}

	uds_log_info("Highest-numbered recovery journal block has sequence number %llu, and the highest-numbered usable block is %llu",
		     (unsigned long long) recovery->highest_tail,
		     (unsigned long long) recovery->tail);

	if (is_replaying(vdo)) {
		/* We need to know how many entries the block map rebuild completion will hold. */
		result = count_increment_entries(recovery);
		if (result != VDO_SUCCESS) {
			vdo_finish_completion(completion, result);
			return;
		}

		/* We need to access the block map from a logical zone. */
		prepare_recovery_completion(recovery,
					    launch_block_map_recovery,
					    VDO_ZONE_TYPE_LOGICAL);
		vdo_load_slab_depot(vdo->depot,
				    VDO_ADMIN_STATE_LOADING_FOR_RECOVERY,
				    completion,
				    recovery);
		return;
	}

	result = compute_usages(recovery);
	if (abort_recovery_on_error(result, recovery))
		return;

	prepare_recovery_completion(recovery, find_slab_journal_entries, VDO_ZONE_TYPE_LOGICAL);
	vdo_invoke_completion_callback(completion);
}

/**
 * launch_recovery() - Construct a recovery completion and launch it.
 * @parent: The completion to notify when the offline portion of the recovery is complete.
 * @journal_data: The contents of the recovery journal
 *
 * Return: VDO_SUCCESS if the recovery was launched, other an error code
 *
 * Applies all valid journal block entries to all vdo structures. This function performs the
 * offline portion of recovering a vdo from a crash.
 */
static int launch_recovery(struct vdo_completion *parent, char *journal_data)
{
	struct vdo *vdo = parent->vdo;
	struct recovery_completion *recovery;
	zone_count_t zone;
	zone_count_t zone_count = vdo->thread_config->physical_zone_count;
	int result;

	result = UDS_ALLOCATE_EXTENDED(struct recovery_completion,
				       zone_count,
				       struct wait_queue,
				       __func__,
				       &recovery);
	if (result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		return result;
	}

	recovery->journal_data = journal_data;
	result = make_int_map(INT_MAP_CAPACITY, 0, &recovery->slot_entry_map);
	if (result != VDO_SUCCESS) {
		free_vdo_recovery_completion(recovery);
		return result;
	}

	for (zone = 0; zone < zone_count; zone++)
		initialize_wait_queue(&recovery->missing_decrefs[zone]);

	vdo_initialize_completion(&recovery->completion, vdo, VDO_RECOVERY_COMPLETION);
	recovery->completion.error_handler = abort_recovery;
	recovery->completion.parent = parent;
	prepare_to_apply_journal_entries(recovery);
	return VDO_SUCCESS;
}

/*--------------------------------------------------------------------*/

/** as_rebuild_completion() - Convert a generic completion to a rebuild_completion. */
static inline struct rebuild_completion * __must_check
as_rebuild_completion(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_READ_ONLY_REBUILD_COMPLETION);
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

	vdo_set_page_cache_rebuild_mode(block_map->zones[0].page_cache, false);
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

	vdo_finish_completion(&rebuild->completion, result);
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
							VDO_JOURNAL_DATA_INCREMENT);
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
	struct block_map_page *page = vdo_dereference_writable_page(completion);

	ASSERT_LOG_ONLY(page != NULL, "page available");

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
		vdo_init_page_completion(page_completion,
					 block_map->zones[0].page_cache,
					 pbn,
					 true,
					 rebuild,
					 page_loaded,
					 handle_page_load_error);
		rebuild->outstanding++;
		/*
		 * Ensure that we don't blow the stack or race with ourselves in the event that all
		 * the pages we request are already in the cache.
		 */
		completion->requeue = true;
		vdo_get_page(completion);
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
							VDO_JOURNAL_BLOCK_MAP_INCREMENT);
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
	struct vdo_page_cache *cache = vdo->block_map->zones[0].page_cache;

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
 * append_sector_entries() - Append an array of recovery journal entries from a journal block
 *                           sector to the array of numbered mappings in the rebuild completion,
 *                           numbering each entry in the order they are appended.
 * @rebuild: The journal rebuild completion.
 * @sector: The recovery journal sector with entries.
 * @entry_count: The number of entries to append.
 */
static void append_sector_entries(struct rebuild_completion *rebuild,
				  struct packed_journal_sector *sector,
				  journal_entry_count_t entry_count)
{
	journal_entry_count_t i;
	struct vdo *vdo = rebuild->completion.vdo;

	for (i = 0; i < entry_count; i++) {
		int result;
		struct recovery_journal_entry entry =
			vdo_unpack_recovery_journal_entry(&sector->entries[i]);

		result = validate_recovery_journal_entry(vdo, &entry);
		if (result != VDO_SUCCESS)
			/* When recovering from read-only mode, ignore damaged entries. */
			continue;

		if (vdo_is_journal_increment_operation(entry.operation)) {
			rebuild->entries[rebuild->entry_count] = (struct numbered_block_mapping) {
				.block_map_slot = entry.slot,
				.block_map_entry = vdo_pack_block_map_entry(entry.mapping.pbn,
									    entry.mapping.state),
				.number = rebuild->entry_count,
			};
			rebuild->entry_count++;
		}
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
	struct vdo *vdo = rebuild->completion.vdo;
	struct recovery_journal *journal = vdo->recovery_journal;
	sequence_number_t first = rebuild->head;
	sequence_number_t last = rebuild->tail;
	block_count_t max_count = ((last - first + 1) * journal->entries_per_block);

	/*
	 * Allocate an array of numbered_block_mapping structures large enough to transcribe every
	 * packed_recovery_journal_entry from every valid journal block.
	 */
	result = UDS_ALLOCATE(max_count,
			      struct numbered_block_mapping,
			      __func__,
			      &rebuild->entries);
	if (result != VDO_SUCCESS)
		return result;

	for (i = first; i <= last; i++) {
		struct recovery_block_header header;
		journal_entry_count_t block_entries;
		u8 j;

		header = get_recovery_journal_block_header(journal, rebuild->journal_data, i);
		if (!is_exact_recovery_journal_block(journal, &header, i))
			/* This block is invalid, so skip it. */
			continue;

		/* Don't extract more than the expected maximum entries per block. */
		block_entries = min(journal->entries_per_block, header.entry_count);
		for (j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
			journal_entry_count_t sector_entries;
			struct packed_journal_sector *sector =
				get_sector(journal, rebuild->journal_data, i, j);

			/* Stop when all entries counted in the header are applied or skipped. */
			if (block_entries == 0)
				break;

			if (!vdo_is_valid_recovery_journal_sector(&header, sector)) {
				block_entries -= min_t(journal_entry_count_t,
						       block_entries,
						       RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
				continue;
			}

			/* Don't extract more than the expected maximum entries per sector. */
			sector_entries = min_t(u8,
					       sector->entry_count,
					       RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);

			/* Only extract as many as the block header calls for. */
			sector_entries = min(sector_entries, block_entries);
			append_sector_entries(rebuild, sector, sector_entries);
			/*
			 * Even if the sector wasn't full, count it as full when counting up to the
			 * entry count the block header claims.
			 */
			block_entries -= min_t(journal_entry_count_t,
					       block_entries,
					       RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
		}
	}

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
	bool found_entries;
	struct rebuild_completion *rebuild = as_rebuild_completion(completion);
	struct vdo *vdo = completion->vdo;

	vdo_assert_on_logical_zone_thread(vdo, 0, __func__);

	found_entries = find_recovery_journal_head_and_tail(vdo->recovery_journal,
							    rebuild->journal_data,
							    &rebuild->tail,
							    &rebuild->head,
							    NULL);
	if (found_entries) {
		int result = extract_journal_entries(rebuild);

		if (abort_rebuild_on_error(result, rebuild))
			return;
	}

	/* Suppress block map errors. */
	vdo_set_page_cache_rebuild_mode(vdo->block_map->zones[0].page_cache, true);

	/* Play the recovery journal into the block map. */
	prepare_rebuild_completion(rebuild,
				   rebuild_reference_counts,
				   completion->callback_thread_id);
	vdo_recover_block_map(vdo, rebuild->entry_count, rebuild->entries, completion);
}

/**
 * launch_rebuild() - Construct a rebuild_completion and launch it.
 * @parent: The completion to notify when the rebuild is complete.
 * @journal_data: The contents of the recovery journal
 *
 * Return: VDO_SUCCESS if the rebuild was launched, other an error code
 *
 * Apply all valid journal block entries to all vdo structures.
 */
static int launch_rebuild(struct vdo_completion *parent, char *journal_data)
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
		return result;
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
	return VDO_SUCCESS;
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
	int result;

	if (++loader->complete != loader->count)
		return;

	uds_log_info("Finished reading recovery journal");
	free_journal_loader(UDS_FORGET(loader));
	if (parent->result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		vdo_complete_completion(parent);
		return;
	}

	result = (vdo_state_requires_recovery(vdo->load_state) ?
		  launch_recovery(parent, journal_data) :
		  launch_rebuild(parent, journal_data));
	if (result != VDO_SUCCESS)
		vdo_finish_completion(parent, result);
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
	physical_block_number_t pbn = vdo_get_fixed_layout_partition_offset(journal->partition);
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
		vdo_finish_completion(parent, result);
		return;
	}

	result = UDS_ALLOCATE_EXTENDED(struct journal_loader,
				       vio_count,
				       struct vio *,
				       __func__,
				       &loader);
	if (result != VDO_SUCCESS) {
		UDS_FREE(ptr);
		vdo_finish_completion(parent, result);
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
			vdo_finish_completion(parent, result);
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
