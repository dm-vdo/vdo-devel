// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "ref-counts.h"

#include <linux/bio.h>
#include <linux/minmax.h>

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "completion.h"
#include "io-submitter.h"
#include "journal-point.h"
#include "physical-zone.h"
#include "read-only-notifier.h"
#include "slab.h"
#include "slab-journal.h"
#include "slab-summary.h"
#include "status-codes.h"
#include "string-utils.h"
#include "vdo.h"
#include "vdo-component-states.h"
#include "vio.h"
#include "wait-queue.h"

static const u64 BYTES_PER_WORD = sizeof(u64);
static const bool NORMAL_OPERATION = true;

/**
 * ref_counts_from_waiter() - Return the ref_counts from the ref_counts waiter.
 * @waiter: The waiter to convert.
 *
 * Return: The ref_counts.
 */
static inline struct ref_counts * __must_check ref_counts_from_waiter(struct waiter *waiter)
{
	if (waiter == NULL)
		return NULL;
	return container_of(waiter, struct ref_counts, slab_summary_waiter);
}

/**
 * index_to_pbn() - Convert the index of a reference counter back to the block number of the
 *                  physical block for which it is counting references.
 * @ref_counts: The reference counts object.
 * @index: The array index of the reference counter.
 *
 * The index is assumed to be valid and in-range.
 *
 * Return: The physical block number corresponding to the index.
 */
static physical_block_number_t index_to_pbn(const struct ref_counts *ref_counts, u64 index)
{
	return (ref_counts->slab->start + index);
}

#ifdef INTERNAL
/**
 * pbn_to_index() - Convert a block number to the index of a reference counter for that block.
 * @ref_counts: The reference counts object.
 * @pbn: The physical block number.
 *
 * Out of range values are pinned to the beginning or one past the end of the array.
 *
 * Return: The index corresponding to the physical block number.
 */
static u64 pbn_to_index(const struct ref_counts *ref_counts, physical_block_number_t pbn)
{
	u64 index;

	if (pbn < ref_counts->slab->start)
		return 0;
	index = (pbn - ref_counts->slab->start);
	return min_t(u64, index, ref_counts->block_count);
}
#endif /* INTERNAL */

/**
 * vdo_reference_count_to_status() - Convert a reference count to a reference status.
 * @count: The count to convert.
 *
 * Return: The appropriate reference status.
 */
static enum reference_status __must_check vdo_reference_count_to_status(vdo_refcount_t count)
{
	if (count == EMPTY_REFERENCE_COUNT)
		return RS_FREE;
	else if (count == 1)
		return RS_SINGLE;
	else if (count == PROVISIONAL_REFERENCE_COUNT)
		return RS_PROVISIONAL;
	else
		return RS_SHARED;
}

/**
 * vdo_reset_search_cursor() - Reset the free block search back to the first reference counter in
 *                             the first reference block.
 * @ref_counts: The ref_counts object containing the search cursor.
 */
void vdo_reset_search_cursor(struct ref_counts *ref_counts)
{
	struct search_cursor *cursor = &ref_counts->search_cursor;

	cursor->block = cursor->first_block;
	cursor->index = 0;
	/* Unit tests have slabs with only one reference block (and it's a runt). */
	cursor->end_index = min_t(u32, COUNTS_PER_BLOCK, ref_counts->block_count);
}

/**
 * advance_search_cursor() - Advance the search cursor to the start of the next reference block.
 * @ref_counts: The ref_counts object containing the search cursor.
 *
 * Wraps around to the first reference block if the current block is the last reference block.
 *
 * Return: true unless the cursor was at the last reference block.
 */
static bool advance_search_cursor(struct ref_counts *ref_counts)
{
	struct search_cursor *cursor = &ref_counts->search_cursor;

	/*
	 * If we just finished searching the last reference block, then wrap back around to the
	 * start of the array.
	 */
	if (cursor->block == cursor->last_block) {
		vdo_reset_search_cursor(ref_counts);
		return false;
	}

	/* We're not already at the end, so advance to cursor to the next block. */
	cursor->block++;
	cursor->index = cursor->end_index;

	if (cursor->block == cursor->last_block)
		/* The last reference block will usually be a runt. */
		cursor->end_index = ref_counts->block_count;
	else
		cursor->end_index += COUNTS_PER_BLOCK;
	return true;
}

/**
 * vdo_make_ref_counts() - Create a reference counting object.
 * @block_count: The number of physical blocks that can be referenced.
 * @slab: The slab of the ref counts object.
 * @origin: The layer PBN at which to save ref_counts.
 * @read_only_notifier: The context for tracking read-only mode.
 * @ref_counts_ptr: The pointer to hold the new ref counts object.
 *
 * A reference counting object can keep a reference count for every physical block in the VDO
 * configuration. Since we expect the vast majority of the blocks to have 0 or 1 reference counts,
 * the structure is optimized for that situation.
 *
 * Return: A success or error code.
 */
int vdo_make_ref_counts(block_count_t block_count,
			struct vdo_slab *slab,
			physical_block_number_t origin,
			struct read_only_notifier *read_only_notifier,
			struct ref_counts **ref_counts_ptr)
{
	size_t index, bytes;
	block_count_t ref_block_count = vdo_get_saved_reference_count_size(block_count);
	struct ref_counts *ref_counts;
	int result;

	result = UDS_ALLOCATE_EXTENDED(struct ref_counts,
				       ref_block_count,
				       struct reference_block,
				       "ref counts structure",
				       &ref_counts);
	if (result != UDS_SUCCESS)
		return result;

	/*
	 * Allocate such that the runt slab has a full-length memory array, plus a little padding
	 * so we can word-search even at the very end.
	 */
	bytes = ((ref_block_count * COUNTS_PER_BLOCK) + (2 * BYTES_PER_WORD));
	result = UDS_ALLOCATE(bytes, vdo_refcount_t, "ref counts array", &ref_counts->counters);
	if (result != UDS_SUCCESS) {
		vdo_free_ref_counts(ref_counts);
		return result;
	}

	ref_counts->slab = slab;
	ref_counts->block_count = block_count;
	ref_counts->free_blocks = block_count;
	ref_counts->origin = origin;
	ref_counts->reference_block_count = ref_block_count;
	ref_counts->read_only_notifier = read_only_notifier;
	ref_counts->statistics = &slab->allocator->ref_counts_statistics;
	ref_counts->search_cursor.first_block = &ref_counts->blocks[0];
	ref_counts->search_cursor.last_block = &ref_counts->blocks[ref_block_count - 1];
	vdo_reset_search_cursor(ref_counts);

	for (index = 0; index < ref_block_count; index++) {
		ref_counts->blocks[index] = (struct reference_block) {
			.ref_counts = ref_counts,
		};
	}

	*ref_counts_ptr = ref_counts;
	return VDO_SUCCESS;
}

/**
 * vdo_free_ref_counts() - Free a reference counting object.
 * @ref_counts: The object to free.
 */
void vdo_free_ref_counts(struct ref_counts *ref_counts)
{
	if (ref_counts == NULL)
		return;

	UDS_FREE(UDS_FORGET(ref_counts->counters));
	UDS_FREE(ref_counts);
}

/**
 * has_active_io() - Check whether a ref_counts object has active I/O.
 * @ref_counts: The ref_counts to check.
 *
 * Return: true if there is reference block I/O or a summary update in progress.
 */
static bool __must_check has_active_io(struct ref_counts *ref_counts)
{
	return ((ref_counts->active_count > 0) || ref_counts->updating_slab_summary);
}

/**
 * vdo_are_ref_counts_active() - Check whether a ref_counts is active.
 * @ref_counts: The ref_counts to check.
 */
bool vdo_are_ref_counts_active(struct ref_counts *ref_counts)
{
	const struct admin_state_code *code;

	if (has_active_io(ref_counts))
		return true;

	/* When not suspending or recovering, the ref_counts must be clean. */
	code = vdo_get_admin_state_code(&ref_counts->slab->state);
	return (has_waiters(&ref_counts->dirty_blocks) &&
		(code != VDO_ADMIN_STATE_SUSPENDING) &&
		(code != VDO_ADMIN_STATE_RECOVERING));
}

static void enter_ref_counts_read_only_mode(struct ref_counts *ref_counts, int result)
{
	vdo_enter_read_only_mode(ref_counts->read_only_notifier, result);
	vdo_check_if_slab_drained(ref_counts->slab);
}

/**
 * dirty_block() - Mark a reference count block as dirty, potentially adding it to the dirty queue
 *                 if it wasn't already dirty.
 * @block: The reference block to mark as dirty.
 */
static void dirty_block(struct reference_block *block)
{
	if (block->is_dirty)
		return;

	block->is_dirty = true;
	if (!block->is_writing)
		enqueue_waiter(&block->ref_counts->dirty_blocks, &block->waiter);
}

/**
 * vdo_get_unreferenced_block_count() - Get the stored count of the number of blocks that are
 *                                      currently free.
 * @ref_counts: The ref_counts object.
 *
 * Return: The number of blocks with a reference count of zero.
 */
block_count_t vdo_get_unreferenced_block_count(struct ref_counts *ref_counts)
{
	return ref_counts->free_blocks;
}

/**
 * vdo_get_reference_block() - Get the reference block that covers the given block index.
 * @ref_counts: The refcounts object.
 * @index: The block index.
 */
EXTERNAL_STATIC struct reference_block * __must_check
vdo_get_reference_block(struct ref_counts *ref_counts, slab_block_number index)
{
	return &ref_counts->blocks[index / COUNTS_PER_BLOCK];
}

/**
 * get_reference_counter() - Get the reference counter that covers the given physical block number.
 * @ref_counts: The refcounts object.
 * @pbn: The physical block number.
 * @counter_ptr: A pointer to the reference counter.
 */
static int get_reference_counter(struct ref_counts *ref_counts,
				 physical_block_number_t pbn,
				 vdo_refcount_t **counter_ptr)
{
	slab_block_number index;
	int result = vdo_slab_block_number_from_pbn(ref_counts->slab, pbn, &index);

	if (result != VDO_SUCCESS)
		return result;

	*counter_ptr = &ref_counts->counters[index];

	return VDO_SUCCESS;
}

/**
 * vdo_get_available_references() - Determine how many times a reference count
 *                                  can be incremented without overflowing.
 * @ref_counts: The ref_counts object.
 * @pbn: The physical block number.
 *
 * Return: The number of increments that can be performed.
 */
u8 vdo_get_available_references(struct ref_counts *ref_counts, physical_block_number_t pbn)
{
	vdo_refcount_t *counter_ptr = NULL;
	int result = get_reference_counter(ref_counts, pbn, &counter_ptr);

	if (result != VDO_SUCCESS)
		return 0;

	if (*counter_ptr == PROVISIONAL_REFERENCE_COUNT)
		return (MAXIMUM_REFERENCE_COUNT - 1);

	return (MAXIMUM_REFERENCE_COUNT - *counter_ptr);
}

/**
 * increment_for_data() - Increment the reference count for a data block.
 * @ref_counts: The ref_counts responsible for the block.
 * @block: The reference block which contains the block being updated.
 * @block_number: The block to update.
 * @old_status: The reference status of the data block before this increment.
 * @lock: The pbn_lock associated with this increment (may be NULL).
 * @counter_ptr: A pointer to the count for the data block (in, out).
 * @free_status_changed: A pointer which will be set to true if this update changed the free status
 *                       of the block.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int increment_for_data(struct ref_counts *ref_counts,
			      struct reference_block *block,
			      slab_block_number block_number,
			      enum reference_status old_status,
			      struct pbn_lock *lock,
			      vdo_refcount_t *counter_ptr,
			      bool *free_status_changed)
{
	switch (old_status) {
	case RS_FREE:
		*counter_ptr = 1;
		block->allocated_count++;
		ref_counts->free_blocks--;
		*free_status_changed = true;
		break;

	case RS_PROVISIONAL:
		*counter_ptr = 1;
		*free_status_changed = false;
		break;

	default:
		/* Single or shared */
		if (*counter_ptr >= MAXIMUM_REFERENCE_COUNT)
			return uds_log_error_strerror(VDO_REF_COUNT_INVALID,
						      "Incrementing a block already having 254 references (slab %u, offset %u)",
						      ref_counts->slab->slab_number,
						      block_number);
		(*counter_ptr)++;
		*free_status_changed = false;
	}

	if (lock != NULL)
		vdo_unassign_pbn_lock_provisional_reference(lock);
	return VDO_SUCCESS;
}

/**
 * decrement_for_data() - Decrement the reference count for a data block.
 * @ref_counts: The ref_counts responsible for the block.
 * @block: The reference block which contains the block being updated.
 * @block_number: The block to update.
 * @old_status: The reference status of the data block before this decrement.
 * @updater: The reference updater doing this operation in case we need to look up the pbn lock.
 * @lock: The pbn_lock associated with the block being decremented (may be NULL).
 * @counter_ptr: A pointer to the count for the data block (in, out).
 * @free_status_changed: A pointer which will be set to true if this update changed the free status
 *                       of the block.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int decrement_for_data(struct ref_counts *ref_counts,
			      struct reference_block *block,
			      slab_block_number block_number,
			      enum reference_status old_status,
			      struct reference_updater *updater,
			      vdo_refcount_t *counter_ptr,
			      bool *free_status_changed)
{
	switch (old_status) {
	case RS_FREE:
		return uds_log_error_strerror(VDO_REF_COUNT_INVALID,
					      "Decrementing free block at offset %u in slab %u",
					      block_number,
					      ref_counts->slab->slab_number);

	case RS_PROVISIONAL:
	case RS_SINGLE:
		if (updater->zpbn.zone != NULL) {
			struct pbn_lock *lock = vdo_get_physical_zone_pbn_lock(updater->zpbn.zone,
									       updater->zpbn.pbn);

			if (lock != NULL) {
				/*
				 * There is a read lock on this block, so the block must not become
				 * unreferenced.
				 */
				*counter_ptr = PROVISIONAL_REFERENCE_COUNT;
				*free_status_changed = false;
				vdo_assign_pbn_lock_provisional_reference(lock);
				break;
			}
		}

		*counter_ptr = EMPTY_REFERENCE_COUNT;
		block->allocated_count--;
		ref_counts->free_blocks++;
		*free_status_changed = true;
		break;

	default:
		/* Shared */
		(*counter_ptr)--;
		*free_status_changed = false;
	}

	return VDO_SUCCESS;
}

/**
 * increment_for_block_map() - Increment the reference count for a block map page.
 * @ref_counts: The ref_counts responsible for the block.
 * @block: The reference block which contains the block being updated.
 * @block_number: The block to update.
 * @old_status: The reference status of the block before this increment.
 * @lock: The pbn_lock associated with this increment (may be NULL).
 * @normal_operation: Whether we are in normal operation vs. recovery or rebuild.
 * @counter_ptr: A pointer to the count for the block (in, out).
 * @free_status_changed: A pointer which will be set to true if this update changed the free status
 *                       of the block.
 *
 * All block map increments should be from provisional to MAXIMUM_REFERENCE_COUNT. Since block map
 * blocks never dedupe they should never be adjusted from any other state. The adjustment always
 * results in MAXIMUM_REFERENCE_COUNT as this value is used to prevent dedupe against block map
 * blocks.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int increment_for_block_map(struct ref_counts *ref_counts,
				   struct reference_block *block,
				   slab_block_number block_number,
				   enum reference_status old_status,
				   struct pbn_lock *lock,
				   bool normal_operation,
				   vdo_refcount_t *counter_ptr,
				   bool *free_status_changed)
{
	switch (old_status) {
	case RS_FREE:
		if (normal_operation)
			return uds_log_error_strerror(VDO_REF_COUNT_INVALID,
						      "Incrementing unallocated block map block (slab %u, offset %u)",
						      ref_counts->slab->slab_number,
						      block_number);

		*counter_ptr = MAXIMUM_REFERENCE_COUNT;
		block->allocated_count++;
		ref_counts->free_blocks--;
		*free_status_changed = true;
		return VDO_SUCCESS;

	case RS_PROVISIONAL:
		if (!normal_operation)
			return uds_log_error_strerror(VDO_REF_COUNT_INVALID,
						      "Block map block had provisional reference during replay (slab %u, offset %u)",
						      ref_counts->slab->slab_number,
						      block_number);

		*counter_ptr = MAXIMUM_REFERENCE_COUNT;
		*free_status_changed = false;
		if (lock != NULL)
			vdo_unassign_pbn_lock_provisional_reference(lock);
		return VDO_SUCCESS;

	default:
		return uds_log_error_strerror(VDO_REF_COUNT_INVALID,
					      "Incrementing a block map block which is already referenced %u times (slab %u, offset %u)",
					      *counter_ptr,
					      ref_counts->slab->slab_number,
					      block_number);
	}
}

/**
 * update_reference_count() - Update the reference count of a block.
 * @ref_counts: The ref_counts responsible for the block.
 * @block: The reference block which contains the block being updated.
 * @block_number: The block to update.
 * @slab_journal_point: The slab journal point at which this update is journaled.
 * @updater: The reference updater.
 * @normal_operation: Whether we are in normal operation vs. recovery or rebuild.
 * @free_status_changed: A pointer which will be set to true if this update changed the free status
 *                       of the block.
 * @provisional_decrement_ptr: A pointer which will be set to true if this update was a decrement
 *                             of a provisional reference.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int
update_reference_count(struct ref_counts *ref_counts,
		       struct reference_block *block,
		       slab_block_number block_number,
		       const struct journal_point *slab_journal_point,
		       struct reference_updater *updater,
		       bool normal_operation,
		       bool *free_status_changed,
		       bool *provisional_decrement_ptr)
{
	vdo_refcount_t *counter_ptr = &ref_counts->counters[block_number];
	enum reference_status old_status = vdo_reference_count_to_status(*counter_ptr);
	int result;

	if (!updater->increment) {
		result = decrement_for_data(ref_counts,
					    block,
					    block_number,
					    old_status,
					    updater,
					    counter_ptr,
					    free_status_changed);
		if ((result == VDO_SUCCESS) && (old_status == RS_PROVISIONAL)) {
			if (provisional_decrement_ptr != NULL)
				*provisional_decrement_ptr = true;
			return VDO_SUCCESS;
		}
	} else if (updater->operation == VDO_JOURNAL_DATA_REMAPPING) {
		result = increment_for_data(ref_counts,
					    block,
					    block_number,
					    old_status,
					    updater->lock,
					    counter_ptr,
					    free_status_changed);
	} else {
		result = increment_for_block_map(ref_counts,
						 block,
						 block_number,
						 old_status,
						 updater->lock,
						 normal_operation,
						 counter_ptr,
						 free_status_changed);
	}

	if (result != VDO_SUCCESS)
		return result;

	if (vdo_is_valid_journal_point(slab_journal_point))
		ref_counts->slab_journal_point = *slab_journal_point;

	return VDO_SUCCESS;
}

/**
 * vdo_adjust_reference_count() - Adjust the reference count of a block.
 * @ref_counts: The refcounts object.
 * @updater: The reference count updater.
 * @slab_journal_point: The slab journal entry for this adjustment.
 * @free_status_changed: A pointer which will be set to true if the free status of the block
 *                       changed.
 *
 * Return: A success or error code, specifically: VDO_REF_COUNT_INVALID if a decrement would result
 *         in a negative reference count, or an increment in a count greater than MAXIMUM_REFS
 */
int vdo_adjust_reference_count(struct ref_counts *ref_counts,
			       struct reference_updater *updater,
			       const struct journal_point *slab_journal_point,
			       bool *free_status_changed)
{
	slab_block_number block_number;
	int result;
	struct reference_block *block;
	bool provisional_decrement = false;

	if (!vdo_is_slab_open(ref_counts->slab))
		return VDO_INVALID_ADMIN_STATE;

	result = vdo_slab_block_number_from_pbn(ref_counts->slab,
						updater->zpbn.pbn,
						&block_number);
	if (result != VDO_SUCCESS)
		return result;

	block = vdo_get_reference_block(ref_counts, block_number);
	result = update_reference_count(ref_counts,
					block,
					block_number,
					slab_journal_point,
					updater,
					NORMAL_OPERATION,
					free_status_changed,
					&provisional_decrement);
	if ((result != VDO_SUCCESS) || provisional_decrement)
		return result;

	if (block->is_dirty && (block->slab_journal_lock > 0)) {
		sequence_number_t entry_lock = slab_journal_point->sequence_number;
		/*
		 * This block is already dirty and a slab journal entry has been made for it since
		 * the last time it was clean. We must release the per-entry slab journal lock for
		 * the entry associated with the update we are now doing.
		 */
		result = ASSERT(vdo_is_valid_journal_point(slab_journal_point),
				"Reference count adjustments need slab journal points.");
		if (result != VDO_SUCCESS)
			return result;

		vdo_adjust_slab_journal_block_reference(ref_counts->slab->journal, entry_lock, -1);
		return VDO_SUCCESS;
	}

	/*
	 * This may be the first time we are applying an update for which there is a slab journal
	 * entry to this block since the block was cleaned. Therefore, we convert the per-entry
	 * slab journal lock to an uncommitted reference block lock, if there is a per-entry lock.
	 */
	if (vdo_is_valid_journal_point(slab_journal_point))
		block->slab_journal_lock = slab_journal_point->sequence_number;
	else
		block->slab_journal_lock = 0;

	dirty_block(block);
	return VDO_SUCCESS;
}

/**
 * vdo_adjust_reference_count_for_rebuild() - Adjust the reference count of a block during rebuild.
 * @ref_counts: The refcounts object.
 * @pbn: The number of the block to adjust.
 * @operation: The operation to perform on the count.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_adjust_reference_count_for_rebuild(struct ref_counts *ref_counts,
					   physical_block_number_t pbn,
					   enum journal_operation operation)
{
	int result;
	slab_block_number block_number;
	struct reference_block *block;
	bool unused_free_status;
	struct reference_updater updater = {
		.operation = operation,
		.increment = true,
	};

	result = vdo_slab_block_number_from_pbn(ref_counts->slab, pbn, &block_number);
	if (result != VDO_SUCCESS)
		return result;

	block = vdo_get_reference_block(ref_counts, block_number);
	result = update_reference_count(ref_counts,
					block,
					block_number,
					NULL,
					&updater,
					!NORMAL_OPERATION,
					&unused_free_status,
					NULL);
	if (result != VDO_SUCCESS)
		return result;

	dirty_block(block);
	return VDO_SUCCESS;
}

/**
 * vdo_replay_reference_count_change() - Replay the reference count adjustment from a slab journal
 *                                       entry into the reference count for a block.
 * @ref_counts: The refcounts object.
 * @entry_point: The slab journal point for the entry.
 * @entry: The slab journal entry being replayed.
 *
 * The adjustment will be ignored if it was already recorded in the reference count.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_replay_reference_count_change(struct ref_counts *ref_counts,
				      const struct journal_point *entry_point,
				      struct slab_journal_entry entry)
{
	bool unused_free_status;
	int result;
	struct reference_block *block = vdo_get_reference_block(ref_counts, entry.sbn);
	sector_count_t sector = (entry.sbn % COUNTS_PER_BLOCK) / COUNTS_PER_SECTOR;
	struct reference_updater updater = {
		.operation = entry.operation,
		.increment = entry.increment,
	};

	if (!vdo_before_journal_point(&block->commit_points[sector], entry_point))
		/* This entry is already reflected in the existing counts, so do nothing. */
		return VDO_SUCCESS;

	/* This entry is not yet counted in the reference counts. */
	result = update_reference_count(ref_counts,
					block,
					entry.sbn,
					entry_point,
					&updater,
					!NORMAL_OPERATION,
					&unused_free_status,
					NULL);
	if (result != VDO_SUCCESS)
		return result;

	dirty_block(block);
	return VDO_SUCCESS;
}

#ifdef INTERNAL
/**
 * vdo_get_reference_status() - Get the reference status of a block.
 * @ref_counts: The refcounts object.
 * @pbn: The physical block number.
 * @status_ptr: Where to put the status of the block.
 *
 * Exposed only for unit testing.
 *
 * Return: A success or error code, specifically: VDO_OUT_OF_RANGE if the pbn is out of range.
 */
int vdo_get_reference_status(struct ref_counts *ref_counts,
			     physical_block_number_t pbn,
			     enum reference_status *status_ptr)
{
	vdo_refcount_t *counter_ptr = NULL;
	int result = get_reference_counter(ref_counts, pbn, &counter_ptr);

	if (result != VDO_SUCCESS)
		return result;

	*status_ptr = vdo_reference_count_to_status(*counter_ptr);
	return VDO_SUCCESS;
}

/**
 * vdo_are_equivalent_ref_counts() - Check whether two reference counters are equivalent.
 * @counter_a: The first counter to compare.
 * @counter_b: The second counter to compare.
 *
 * This method is used for unit testing.
 *
 * Return: true if the two counters are equivalent.
 */
bool vdo_are_equivalent_ref_counts(struct ref_counts *counter_a, struct ref_counts *counter_b)
{
	size_t i;

	if ((counter_a->block_count != counter_b->block_count) ||
	    (counter_a->free_blocks != counter_b->free_blocks) ||
	    (counter_a->reference_block_count != counter_b->reference_block_count))
		return false;

	for (i = 0; i < counter_a->reference_block_count; i++) {
		struct reference_block *block_a = &counter_a->blocks[i];
		struct reference_block *block_b = &counter_b->blocks[i];

		if (block_a->allocated_count != block_b->allocated_count)
			return false;
	}

	return (memcmp(counter_a->counters,
		       counter_b->counters,
		       sizeof(vdo_refcount_t) * counter_a->block_count) == 0);
}
#endif /* INTERNAL */

/**
 * find_zero_byte_in_word() - Find the array index of the first zero byte in word-sized range of
 *                            reference counters.
 * @word_ptr: A pointer to the eight counter bytes to check.
 * @start_index: The array index corresponding to word_ptr[0].
 * @fail_index: The array index to return if no zero byte is found.
 *
 * The search does no bounds checking; the function relies on the array being sufficiently padded.
 *
 * Return: The array index of the first zero byte in the word, or the value passed as fail_index if
 *         no zero byte was found.
 */
static inline slab_block_number
find_zero_byte_in_word(const u8 *word_ptr,
		       slab_block_number start_index,
		       slab_block_number fail_index)
{
	u64 word = get_unaligned_le64(word_ptr);

	/* This looks like a loop, but GCC will unroll the eight iterations for us. */
	unsigned int offset;

	for (offset = 0; offset < BYTES_PER_WORD; offset++) {
		/* Assumes little-endian byte order, which we have on X86. */
		if ((word & 0xFF) == 0)
			return (start_index + offset);
		word >>= 8;
	}

	return fail_index;
}

/**
 * vdo_find_free_block() - Find the first block with a reference count of zero in the specified
 *                         range of reference counter indexes.
 * @ref_counts: The reference counters to scan.
 * @start_index: The array index at which to start scanning (included in the scan).
 * @end_index: The array index at which to stop scanning (excluded from the scan).
 * @index_ptr: A pointer to hold the array index of the free block.
 *
 * Exposed for unit testing.
 *
 * Return: true if a free block was found in the specified range.
 */
EXTERNAL_STATIC
bool vdo_find_free_block(const struct ref_counts *ref_counts,
			 slab_block_number start_index,
			 slab_block_number end_index,
			 slab_block_number *index_ptr)
{
	slab_block_number zero_index;
	slab_block_number next_index = start_index;
	u8 *next_counter = &ref_counts->counters[next_index];
	u8 *end_counter = &ref_counts->counters[end_index];

	/*
	 * Search every byte of the first unaligned word. (Array is padded so reading past end is
	 * safe.)
	 */
	zero_index = find_zero_byte_in_word(next_counter, next_index, end_index);
	if (zero_index < end_index) {
		*index_ptr = zero_index;
		return true;
	}

	/*
	 * On architectures where unaligned word access is expensive, this would be a good place to
	 * advance to an alignment boundary.
	 */
	next_index += BYTES_PER_WORD;
	next_counter += BYTES_PER_WORD;

	/*
	 * Now we're word-aligned; check an word at a time until we find a word containing a zero.
	 * (Array is padded so reading past end is safe.)
	 */
	while (next_counter < end_counter) {
		/*
		 * The following code is currently an exact copy of the code preceding the loop,
		 * but if you try to merge them by using a do loop, it runs slower because a jump
		 * instruction gets added at the start of the iteration.
		 */
		zero_index = find_zero_byte_in_word(next_counter, next_index, end_index);
		if (zero_index < end_index) {
			*index_ptr = zero_index;
			return true;
		}

		next_index += BYTES_PER_WORD;
		next_counter += BYTES_PER_WORD;
	}

	return false;
}

/**
 * search_current_reference_block() - Search the reference block currently saved in the search
 *                                    cursor for a reference count of zero, starting at the saved
 *                                    counter index.
 * @ref_counts: The ref_counts object to search.
 * @free_index_ptr: A pointer to receive the array index of the zero reference count.
 *
 * Return: true if an unreferenced counter was found.
 */
static bool search_current_reference_block(const struct ref_counts *ref_counts,
					   slab_block_number *free_index_ptr)
{
	/* Don't bother searching if the current block is known to be full. */
	return ((ref_counts->search_cursor.block->allocated_count < COUNTS_PER_BLOCK) &&
		vdo_find_free_block(ref_counts,
				    ref_counts->search_cursor.index,
				    ref_counts->search_cursor.end_index,
				    free_index_ptr));
}

/**
 * search_reference_blocks() - Search each reference block for a reference count of zero.
 * @ref_counts: The ref_counts object to search.
 * @free_index_ptr: A pointer to receive the array index of the zero reference count.
 *
 * Searches each reference block for a reference count of zero, starting at the reference block and
 * counter index saved in the search cursor and searching up to the end of the last reference
 * block. The search does not wrap.
 *
 * Return: true if an unreferenced counter was found.
 */
static bool
search_reference_blocks(struct ref_counts *ref_counts, slab_block_number *free_index_ptr)
{
	/* Start searching at the saved search position in the current block. */
	if (search_current_reference_block(ref_counts, free_index_ptr))
		return true;

	/* Search each reference block up to the end of the slab. */
	while (advance_search_cursor(ref_counts))
		if (search_current_reference_block(ref_counts, free_index_ptr))
			return true;

	return false;
}

/**
 * make_provisional_reference() - Do the bookkeeping for making a provisional reference.
 * @ref_counts: The ref_counts.
 * @block_number: The block to reference.
 */
static void
make_provisional_reference(struct ref_counts *ref_counts, slab_block_number block_number)
{
	struct reference_block *block = vdo_get_reference_block(ref_counts, block_number);

	/*
	 * Make the initial transition from an unreferenced block to a
	 * provisionally allocated block.
	 */
	ref_counts->counters[block_number] = PROVISIONAL_REFERENCE_COUNT;

	/* Account for the allocation. */
	block->allocated_count++;
	ref_counts->free_blocks--;
}

/**
 * vdo_allocate_unreferenced_block() - Find a block with a reference count of zero in the range of
 *                                     physical block numbers tracked by the reference counter.
 * @ref_counts: The reference counters to scan.
 * @allocated_ptr: A pointer to hold the physical block number of the block that was found and
 *                 allocated.
 *
 * If a free block is found, that block is allocated by marking it as provisionally referenced, and
 * the allocated block number is returned.
 *
 * Return: VDO_SUCCESS if a free block was found and allocated; VDO_NO_SPACE if there are no
 *         unreferenced blocks; otherwise an error code.
 */
int vdo_allocate_unreferenced_block(struct ref_counts *ref_counts,
				    physical_block_number_t *allocated_ptr)
{
	slab_block_number free_index;

	if (!vdo_is_slab_open(ref_counts->slab))
		return VDO_INVALID_ADMIN_STATE;

	if (!search_reference_blocks(ref_counts, &free_index))
		return VDO_NO_SPACE;

	ASSERT_LOG_ONLY((ref_counts->counters[free_index] == EMPTY_REFERENCE_COUNT),
			"free block must have ref count of zero");
	make_provisional_reference(ref_counts, free_index);

	/*
	 * Update the search hint so the next search will start at the array index just past the
	 * free block we just found.
	 */
	ref_counts->search_cursor.index = (free_index + 1);

	*allocated_ptr = index_to_pbn(ref_counts, free_index);
	return VDO_SUCCESS;
}

/**
 * vdo_provisionally_reference_block() - Provisionally reference a block if it is unreferenced.
 * @ref_counts: The reference counters.
 * @pbn: The PBN to reference.
 * @lock: The pbn_lock on the block (may be NULL).
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_provisionally_reference_block(struct ref_counts *ref_counts,
				      physical_block_number_t pbn,
				      struct pbn_lock *lock)
{
	slab_block_number block_number;
	int result;

	if (!vdo_is_slab_open(ref_counts->slab))
		return VDO_INVALID_ADMIN_STATE;

	result = vdo_slab_block_number_from_pbn(ref_counts->slab, pbn, &block_number);
	if (result != VDO_SUCCESS)
		return result;

	if (ref_counts->counters[block_number] == EMPTY_REFERENCE_COUNT) {
		make_provisional_reference(ref_counts, block_number);
		if (lock != NULL)
			vdo_assign_pbn_lock_provisional_reference(lock);
	}

	return VDO_SUCCESS;
}

#ifdef INTERNAL
/**
 * vdo_count_unreferenced_blocks() - Count all unreferenced blocks in a range [start_block,
 *                                   end_block) of physical block numbers.
 * @ref_counts: The reference counters to scan.
 * @start_pbn: The physical block number at which to start scanning (included in the scan).
 * @end_pbn: The physical block number at which to stop scanning (excluded from the scan).
 *
 * Return: The number of unreferenced blocks.
 */
block_count_t vdo_count_unreferenced_blocks(struct ref_counts *ref_counts,
					    physical_block_number_t start_pbn,
					    physical_block_number_t end_pbn)
{
	block_count_t free_blocks = 0;
	slab_block_number start_index = pbn_to_index(ref_counts, start_pbn);
	slab_block_number end_index = pbn_to_index(ref_counts, end_pbn);
	slab_block_number index;

	for (index = start_index; index < end_index; index++)
		if (ref_counts->counters[index] == EMPTY_REFERENCE_COUNT)
			free_blocks++;

	return free_blocks;
}
#endif /* INTERNAL */

/**
 * Waiter_as_reference_block() - Convert a reference_block's generic wait queue entry back into the
 *                               reference_block.
 * @waiter: The wait queue entry to convert.
 *
 * Return: The wrapping reference_block.
 */
static inline struct reference_block *waiter_as_reference_block(struct waiter *waiter)
{
	return container_of(waiter, struct reference_block, waiter);
}

#ifdef INTERNAL
/**
 * clear_dirty_reference_blocks() - A waiter_callback to clean dirty reference blocks when
 *                                  resetting.
 * @block_waiter: The dirty block.
 * @context: Unused.
 */
static void
clear_dirty_reference_blocks(struct waiter *block_waiter, void *context __always_unused)
{
	waiter_as_reference_block(block_waiter)->is_dirty = false;
}

/**
 * vdo_reset_reference_counts() - Reset all reference counts back to RS_FREE.
 * @ref_counts: The reference counters to reset.
 */
void vdo_reset_reference_counts(struct ref_counts *ref_counts)
{
	size_t i;

	memset(ref_counts->counters, 0, ref_counts->block_count * sizeof(vdo_refcount_t));
	ref_counts->free_blocks = ref_counts->block_count;
	ref_counts->slab_journal_point = (struct journal_point) {
		.sequence_number = 0,
		.entry_count = 0,
	};

	for (i = 0; i < ref_counts->reference_block_count; i++)
		ref_counts->blocks[i].allocated_count = 0;

	notify_all_waiters(&ref_counts->dirty_blocks, clear_dirty_reference_blocks, NULL);
}
#endif /* INTERNAL */

/** finish_summary_update() - A waiter callback that resets the writing state of ref_counts. */
static void finish_summary_update(struct waiter *waiter, void *context)
{
	struct ref_counts *ref_counts = ref_counts_from_waiter(waiter);
	int result = *((int *)context);

	ref_counts->updating_slab_summary = false;

	if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
		vdo_check_if_slab_drained(ref_counts->slab);
		return;
	}

	uds_log_error_strerror(result, "failed to update slab summary");
	enter_ref_counts_read_only_mode(ref_counts, result);
}

/**
 * update_slab_summary_as_clean() - Update slab summary that the ref_counts object is clean.
 * @ref_counts: The ref_counts object that is being written.
 */
static void update_slab_summary_as_clean(struct ref_counts *ref_counts)
{
	tail_block_offset_t offset;
	struct slab_summary_zone *summary = ref_counts->slab->allocator->summary;

	if (summary == NULL)
		return;

	/* Update the slab summary to indicate this ref_counts is clean. */
	offset = vdo_get_summarized_tail_block_offset(summary, ref_counts->slab->slab_number);
	ref_counts->updating_slab_summary = true;
	ref_counts->slab_summary_waiter.callback = finish_summary_update;
	vdo_update_slab_summary_entry(summary,
				      &ref_counts->slab_summary_waiter,
				      ref_counts->slab->slab_number,
				      offset,
				      true,
				      true,
				      get_slab_free_block_count(ref_counts->slab));
}

/**
 * handle_io_error() - Handle an I/O error reading or writing a reference count block.
 * @completion: The VIO doing the I/O as a completion.
 */
static void handle_io_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio *vio = as_vio(completion);
	struct ref_counts *ref_counts =
		((struct reference_block *) completion->parent)->ref_counts;

	record_metadata_io_error(vio);
	return_vio_to_pool(ref_counts->slab->allocator->vio_pool, vio_as_pooled_vio(vio));
	ref_counts->active_count--;
	enter_ref_counts_read_only_mode(ref_counts, result);
}

/**
 * finish_reference_block_write() - After a reference block has written, clean it, release its
 *                                  locks, and return its VIO to the pool.
 * @completion: The VIO that just finished writing.
 */
static void finish_reference_block_write(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = vio_as_pooled_vio(vio);
	struct reference_block *block = completion->parent;
	struct ref_counts *ref_counts = block->ref_counts;

	ref_counts->active_count--;

	/* Release the slab journal lock. */
	vdo_adjust_slab_journal_block_reference(ref_counts->slab->journal,
						block->slab_journal_lock_to_release,
						-1);
	return_vio_to_pool(ref_counts->slab->allocator->vio_pool, pooled);

	/*
	 * We can't clear the is_writing flag earlier as releasing the slab journal lock may cause
	 * us to be dirtied again, but we don't want to double enqueue.
	 */
	block->is_writing = false;

	if (vdo_is_read_only(ref_counts->read_only_notifier)) {
		vdo_check_if_slab_drained(ref_counts->slab);
		return;
	}

	/* Re-queue the block if it was re-dirtied while it was writing. */
	if (block->is_dirty) {
		enqueue_waiter(&block->ref_counts->dirty_blocks, &block->waiter);
		if (vdo_is_slab_draining(ref_counts->slab))
			/* We must be saving, and this block will otherwise not be relaunched. */
			vdo_save_dirty_reference_blocks(ref_counts);

		return;
	}

	/*
	 * Mark the ref_counts as clean in the slab summary if there are no dirty or writing blocks
	 * and no summary update in progress.
	 */
	if (!has_active_io(ref_counts) && !has_waiters(&ref_counts->dirty_blocks))
		update_slab_summary_as_clean(ref_counts);
}

/**
 * vdo_get_reference_counters_for_block() - Find the reference counters for a given block.
 * @block: The reference_block in question.
 *
 * Return: A pointer to the reference counters for this block.
 */
EXTERNAL_STATIC vdo_refcount_t * __must_check
vdo_get_reference_counters_for_block(struct reference_block *block)
{
	size_t block_index = block - block->ref_counts->blocks;

	return &block->ref_counts->counters[block_index * COUNTS_PER_BLOCK];
}

/**
 * vdo_pack_reference_block() - Copy data from a reference block to a buffer ready to be written
 *                              out.
 * @block: The block to copy.
 * @buffer: The char buffer to fill with the packed block.
 */
EXTERNAL_STATIC void vdo_pack_reference_block(struct reference_block *block, void *buffer)
{
	struct packed_reference_block *packed = buffer;
	vdo_refcount_t *counters = vdo_get_reference_counters_for_block(block);
	sector_count_t i;
	struct packed_journal_point commit_point;

	vdo_pack_journal_point(&block->ref_counts->slab_journal_point, &commit_point);

	for (i = 0; i < VDO_SECTORS_PER_BLOCK; i++) {
		packed->sectors[i].commit_point = commit_point;
		memcpy(packed->sectors[i].counts,
		       counters + (i * COUNTS_PER_SECTOR),
		       (sizeof(vdo_refcount_t) * COUNTS_PER_SECTOR));
	}
}

static void write_reference_block_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct reference_block *block = vio->completion.parent;
	thread_id_t thread_id = block->ref_counts->slab->allocator->thread_id;

	continue_vio_after_io(vio, finish_reference_block_write, thread_id);
}

/**
 * write_reference_block() - After a dirty block waiter has gotten a VIO from the VIO pool, copy
 *                           its counters and associated data into the VIO, and launch the write.
 * @waiter: The waiter of the dirty block.
 * @context: The VIO returned by the pool.
 */
static void write_reference_block(struct waiter *waiter, void *context)
{
	size_t block_offset;
	physical_block_number_t pbn;
	struct pooled_vio *pooled = context;
	struct vdo_completion *completion = &pooled->vio.completion;
	struct reference_block *block = waiter_as_reference_block(waiter);

	vdo_pack_reference_block(block, pooled->vio.data);
	block_offset = (block - block->ref_counts->blocks);
	pbn = (block->ref_counts->origin + block_offset);
	block->slab_journal_lock_to_release = block->slab_journal_lock;
	completion->parent = block;

	/*
	 * Mark the block as clean, since we won't be committing any updates that happen after this
	 * moment. As long as VIO order is preserved, two VIOs updating this block at once will not
	 * cause complications.
	 */
	block->is_dirty = false;

	/*
	 * Flush before writing to ensure that the recovery journal and slab journal entries which
	 * cover this reference update are stable (VDO-2331).
	 */
	WRITE_ONCE(block->ref_counts->statistics->blocks_written,
		   block->ref_counts->statistics->blocks_written + 1);

	completion->callback_thread_id = ((struct block_allocator *) pooled->context)->thread_id;
	submit_metadata_vio(&pooled->vio,
			    pbn,
			    write_reference_block_endio,
			    handle_io_error,
			    REQ_OP_WRITE | REQ_PREFLUSH);
}

/**
 * launch_reference_block_write() - Launch the write of a dirty reference block by first acquiring
 *                                  a VIO for it from the pool.
 * @waiter: The waiter of the block which is starting to write.
 * @context: The parent ref_counts of the block.
 *
 * This can be asynchronous since the writer will have to wait if all VIOs in the pool are
 * currently in use.
 */
static void launch_reference_block_write(struct waiter *waiter, void *context)
{
	struct reference_block *block;
	struct ref_counts *ref_counts = context;

	if (vdo_is_read_only(ref_counts->read_only_notifier))
		return;

	ref_counts->active_count++;
	block = waiter_as_reference_block(waiter);
	block->is_writing = true;
	waiter->callback = write_reference_block;
	acquire_vio_from_pool(ref_counts->slab->allocator->vio_pool, waiter);
}

/**
 * vdo_save_oldest_reference_block() - Request a ref_counts object save its oldest dirty block
 *                                     asynchronously.
 * @ref_counts: The ref_counts object to notify.
 */
EXTERNAL_STATIC void vdo_save_oldest_reference_block(struct ref_counts *ref_counts)
{
	notify_next_waiter(&ref_counts->dirty_blocks, launch_reference_block_write, ref_counts);
}

/**
 * vdo_save_several_reference_blocks() - Request a ref_counts object save several dirty blocks
 *                                       asynchronously.
 * @ref_counts: The ref_counts object to notify.
 * @flush_divisor: The inverse fraction of the dirty blocks to write.
 *
 * This function currently writes 1 / flush_divisor of the dirty blocks.
 */
void vdo_save_several_reference_blocks(struct ref_counts *ref_counts, size_t flush_divisor)
{
	block_count_t written, blocks_to_write;
	block_count_t dirty_block_count = count_waiters(&ref_counts->dirty_blocks);

	if (dirty_block_count == 0)
		return;

	blocks_to_write = dirty_block_count / flush_divisor;
	/* Always save at least one block. */
	if (blocks_to_write == 0)
		blocks_to_write = 1;

	for (written = 0; written < blocks_to_write; written++)
		vdo_save_oldest_reference_block(ref_counts);
}

/**
 * vdo_save_dirty_reference_blocks() - Ask a ref_counts object to save all its dirty blocks
 *                                     asynchronously.
 * @ref_counts: The ref_counts object to notify.
 */
void vdo_save_dirty_reference_blocks(struct ref_counts *ref_counts)
{
	notify_all_waiters(&ref_counts->dirty_blocks, launch_reference_block_write, ref_counts);
	vdo_check_if_slab_drained(ref_counts->slab);
}

/**
 * vdo_dirty_all_reference_blocks() - Mark all reference count blocks as dirty.
 * @ref_counts: The ref_counts of the reference blocks.
 */
void vdo_dirty_all_reference_blocks(struct ref_counts *ref_counts)
{
	block_count_t i;

	for (i = 0; i < ref_counts->reference_block_count; i++)
		dirty_block(&ref_counts->blocks[i]);
}

/**
 * clear_provisional_references() - Clear the provisional reference counts from a reference block.
 * @block: The block to clear.
 */
static void clear_provisional_references(struct reference_block *block)
{
	vdo_refcount_t *counters = vdo_get_reference_counters_for_block(block);
	block_count_t j;

	for (j = 0; j < COUNTS_PER_BLOCK; j++) {
		if (counters[j] == PROVISIONAL_REFERENCE_COUNT) {
			counters[j] = EMPTY_REFERENCE_COUNT;
			block->allocated_count--;
		}
	}
}

/**
 * unpack_reference_block() - Unpack reference counts blocks into the internal memory structure.
 * @packed: The written reference block to be unpacked.
 * @block: The internal reference block to be loaded.
 */
static void
unpack_reference_block(struct packed_reference_block *packed, struct reference_block *block)
{
	block_count_t index;
	sector_count_t i;
	struct ref_counts *ref_counts = block->ref_counts;
	vdo_refcount_t *counters = vdo_get_reference_counters_for_block(block);

	for (i = 0; i < VDO_SECTORS_PER_BLOCK; i++) {
		struct packed_reference_sector *sector = &packed->sectors[i];

		vdo_unpack_journal_point(&sector->commit_point, &block->commit_points[i]);
		memcpy(counters + (i * COUNTS_PER_SECTOR),
		       sector->counts,
		       (sizeof(vdo_refcount_t) * COUNTS_PER_SECTOR));
		/* The slab_journal_point must be the latest point found in any sector. */
		if (vdo_before_journal_point(&ref_counts->slab_journal_point,
					     &block->commit_points[i]))
			ref_counts->slab_journal_point = block->commit_points[i];

		if ((i > 0) && !vdo_are_equivalent_journal_points(&block->commit_points[0],
								  &block->commit_points[i])) {
			size_t block_index = block - block->ref_counts->blocks;

			uds_log_warning("Torn write detected in sector %u of reference block %zu of slab %u",
					i,
					block_index,
					block->ref_counts->slab->slab_number);
		}
	}

	block->allocated_count = 0;
	for (index = 0; index < COUNTS_PER_BLOCK; index++)
		if (counters[index] != EMPTY_REFERENCE_COUNT)
			block->allocated_count++;
}

/**
 * finish_reference_block_load() - After a reference block has been read, unpack it.
 * @completion: The VIO that just finished reading.
 */
static void finish_reference_block_load(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = vio_as_pooled_vio(vio);
	struct reference_block *block = completion->parent;
	struct ref_counts *ref_counts = block->ref_counts;

	unpack_reference_block((struct packed_reference_block *) vio->data, block);
	return_vio_to_pool(ref_counts->slab->allocator->vio_pool, pooled);
	ref_counts->active_count--;
	clear_provisional_references(block);

	ref_counts->free_blocks -= block->allocated_count;
	vdo_check_if_slab_drained(block->ref_counts->slab);
}

static void load_reference_block_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct reference_block *block = vio->completion.parent;

	continue_vio_after_io(vio,
			      finish_reference_block_load,
			      block->ref_counts->slab->allocator->thread_id);
}

/**
 * load_reference_block() - After a block waiter has gotten a VIO from the VIO pool, load the
 *                          block.
 * @waiter: The waiter of the block to load.
 * @context: The VIO returned by the pool.
 */
static void load_reference_block(struct waiter *waiter, void *context)
{
	struct pooled_vio *pooled = context;
	struct vio *vio = &pooled->vio;
	struct reference_block *block = waiter_as_reference_block(waiter);
	size_t block_offset = (block - block->ref_counts->blocks);

	vio->completion.parent = block;
	submit_metadata_vio(vio,
			    block->ref_counts->origin + block_offset,
			    load_reference_block_endio,
			    handle_io_error,
			    REQ_OP_READ);
}

/**
 * load_reference_blocks() - Load reference blocks from the underlying storage into a pre-allocated
 *                           reference counter.
 * @ref_counts: The reference counter to be loaded.
 */
static void load_reference_blocks(struct ref_counts *ref_counts)
{
	block_count_t i;

	ref_counts->free_blocks = ref_counts->block_count;
	ref_counts->active_count = ref_counts->reference_block_count;
	for (i = 0; i < ref_counts->reference_block_count; i++) {
		struct waiter *waiter = &ref_counts->blocks[i].waiter;

		waiter->callback = load_reference_block;
		acquire_vio_from_pool(ref_counts->slab->allocator->vio_pool, waiter);
	}
}

/**
 * vdo_drain_ref_counts() - Drain all reference count I/O.
 * @ref_counts: The reference counts to drain.
 *
 * Depending upon the type of drain being performed (as recorded in the ref_count's vdo_slab), the
 * reference blocks may be loaded from disk or dirty reference blocks may be written out.
 */
void vdo_drain_ref_counts(struct ref_counts *ref_counts)
{
	struct vdo_slab *slab = ref_counts->slab;
	bool save = false;
	const struct admin_state_code *state = vdo_get_admin_state_code(&slab->state);

	if ((state == VDO_ADMIN_STATE_RECOVERING) || (state == VDO_ADMIN_STATE_SUSPENDING))
		return;

	if (state == VDO_ADMIN_STATE_SCRUBBING) {
		if (vdo_must_load_ref_counts(slab->allocator->summary, slab->slab_number)) {
			load_reference_blocks(ref_counts);
			return;
		}
	} else if (state == VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING) {
		if (!vdo_must_load_ref_counts(slab->allocator->summary, slab->slab_number))
			/* These reference counts were never written, so mark them all dirty. */
			vdo_dirty_all_reference_blocks(ref_counts);

		save = true;
	} else if (state == VDO_ADMIN_STATE_REBUILDING) {
		if (vdo_should_save_fully_built_slab(slab)) {
			vdo_dirty_all_reference_blocks(ref_counts);
			save = true;
		}
	} else if (state == VDO_ADMIN_STATE_SAVING) {
		save = (slab->status == VDO_SLAB_REBUILT);
	} else {
		vdo_notify_slab_ref_counts_are_drained(slab, VDO_SUCCESS);
		return;
	}

	if (save)
		vdo_save_dirty_reference_blocks(ref_counts);
}

/**
 * vdo_acquire_dirty_block_locks() - Mark all reference count blocks dirty and cause them to hold
 *                                   locks on slab journal block 1.
 * @ref_counts: The ref_counts of the reference blocks.
 */
void vdo_acquire_dirty_block_locks(struct ref_counts *ref_counts)
{
	block_count_t i;

	vdo_dirty_all_reference_blocks(ref_counts);
	for (i = 0; i < ref_counts->reference_block_count; i++)
		ref_counts->blocks[i].slab_journal_lock = 1;

	vdo_adjust_slab_journal_block_reference(ref_counts->slab->journal, 1,
						ref_counts->reference_block_count);
}

/**
 * vdo_dump_ref_counts() - Dump information about this ref_counts structure.
 * @ref_counts: The ref_counts to dump.
 */
void vdo_dump_ref_counts(const struct ref_counts *ref_counts)
{
	/* Terse because there are a lot of slabs to dump and syslog is lossy. */
	uds_log_info("  ref_counts: free=%u/%u blocks=%u dirty=%zu active=%zu journal@(%llu,%u)%s",
		     ref_counts->free_blocks,
		     ref_counts->block_count,
		     ref_counts->reference_block_count,
		     count_waiters(&ref_counts->dirty_blocks),
		     ref_counts->active_count,
		     (unsigned long long) ref_counts->slab_journal_point.sequence_number,
		     ref_counts->slab_journal_point.entry_count,
		     (ref_counts->updating_slab_summary ? " updating" : ""));
}
