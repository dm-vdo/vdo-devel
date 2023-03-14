// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "slab-depot.h"

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/log2.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "action-manager.h"
#include "admin-state.h"
#include "completion.h"
#include "constants.h"
#include "encodings.h"
#include "heap.h"
#include "io-submitter.h"
#include "priority-table.h"
#include "recovery.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-journal.h"
#include "status-codes.h"
#include "thread-config.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"

struct slab_journal_eraser {
	struct vdo_completion *parent;
	struct dm_kcopyd_client *client;
	block_count_t blocks;
	struct slab_iterator slabs;
};

/**
 * initiate_slab_action() - Initiate a slab action.
 *
 * Implements vdo_admin_initiator.
 */
EXTERNAL_STATIC void initiate_slab_action(struct admin_state *state)
{
	struct vdo_slab *slab = container_of(state, struct vdo_slab, state);

	if (vdo_is_state_draining(state)) {
		const struct admin_state_code *operation = vdo_get_admin_state_code(state);

		if (operation == VDO_ADMIN_STATE_SCRUBBING)
			slab->status = VDO_SLAB_REBUILDING;

		vdo_drain_slab_journal(slab->journal);

		if (slab->reference_counts != NULL)
			vdo_drain_ref_counts(slab->reference_counts);

		vdo_check_if_slab_drained(slab);
		return;
	}

	if (vdo_is_state_loading(state)) {
		vdo_decode_slab_journal(slab->journal);
		return;
	}

	if (vdo_is_state_resuming(state)) {
		vdo_queue_slab(slab);
		vdo_finish_resuming(state);
		return;
	}

	vdo_finish_operation(state, VDO_INVALID_ADMIN_STATE);
}

/**
 * get_next_slab() - Get the next slab to scrub.
 * @scrubber: The slab scrubber.
 *
 * Return: The next slab to scrub or NULL if there are none.
 */
static struct vdo_slab *get_next_slab(struct slab_scrubber *scrubber)
{
	struct vdo_slab *slab;

	slab = list_first_entry_or_null(&scrubber->high_priority_slabs,
					struct vdo_slab,
					allocq_entry);
	if (slab != NULL)
		return slab;

	return list_first_entry_or_null(&scrubber->slabs, struct vdo_slab, allocq_entry);
}

/**
 * has_slabs_to_scrub() - Check whether a scrubber has slabs to scrub.
 * @scrubber: The scrubber to check.
 *
 * Return: true if the scrubber has slabs to scrub.
 */
static bool __must_check has_slabs_to_scrub(struct slab_scrubber *scrubber)
{
	return (get_next_slab(scrubber) != NULL);
}

/**
 * vdo_register_slab_for_scrubbing() - Register a slab with a scrubber.
 * @slab: The slab to scrub.
 * @high_priority: true if the slab should be put on the high-priority queue.
 */
void vdo_register_slab_for_scrubbing(struct vdo_slab *slab, bool high_priority)
{
	struct slab_scrubber *scrubber = &slab->allocator->scrubber;

	ASSERT_LOG_ONLY((slab->status != VDO_SLAB_REBUILT), "slab to be scrubbed is unrecovered");

	if (slab->status != VDO_SLAB_REQUIRES_SCRUBBING)
		return;

	list_del_init(&slab->allocq_entry);
	if (!slab->was_queued_for_scrubbing) {
		WRITE_ONCE(scrubber->slab_count, scrubber->slab_count + 1);
		slab->was_queued_for_scrubbing = true;
	}

	if (high_priority) {
		slab->status = VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING;
		list_add_tail(&slab->allocq_entry, &scrubber->high_priority_slabs);
		return;
	}

	list_add_tail(&slab->allocq_entry, &scrubber->slabs);
}

/**
 * uninitialize_scrubber_vio() - Clean up the slab_scrubber's vio.
 * @scrubber: The scrubber.
 */
static void uninitialize_scrubber_vio(struct slab_scrubber *scrubber)
{
	UDS_FREE(UDS_FORGET(scrubber->vio.data));
	free_vio_components(&scrubber->vio);
}

/**
 * finish_scrubbing() - Stop scrubbing, either because there are no more slabs to scrub or because
 *                      there's been an error.
 * @scrubber: The scrubber.
 */
static void finish_scrubbing(struct slab_scrubber *scrubber, int result)
{
	bool notify = vdo_has_waiters(&scrubber->waiters);
	bool done = !has_slabs_to_scrub(scrubber);
	struct block_allocator *allocator =
		container_of(scrubber, struct block_allocator, scrubber);

	if (done)
		uninitialize_scrubber_vio(scrubber);

	if (scrubber->high_priority_only) {
		scrubber->high_priority_only = false;
		vdo_fail_completion(UDS_FORGET(scrubber->vio.completion.parent), result);
	} else if (done && (atomic_add_return(-1, &allocator->depot->zones_to_scrub) == 0)) {
		/* All of our slabs were scrubbed, and we're the last allocator to finish. */
		enum vdo_state prior_state =
			atomic_cmpxchg(&allocator->depot->vdo->state, VDO_RECOVERING, VDO_DIRTY);

		/*
		 * To be safe, even if the CAS failed, ensure anything that follows is ordered with
		 * respect to whatever state change did happen.
		 */
		smp_mb__after_atomic();

		/*
		 * We must check the VDO state here and not the depot's read_only_notifier since
		 * the compare-swap-above could have failed due to a read-only entry which our own
		 * thread does not yet know about.
		 */
		if (prior_state == VDO_DIRTY)
			uds_log_info("VDO commencing normal operation");
		else if (prior_state == VDO_RECOVERING)
			uds_log_info("Exiting recovery mode");
	}

	/*
	 * Note that the scrubber has stopped, and inform anyone who might be waiting for that to
	 * happen.
	 */
	if (!vdo_finish_draining(&scrubber->admin_state))
		WRITE_ONCE(scrubber->admin_state.current_state, VDO_ADMIN_STATE_SUSPENDED);

	/*
	 * We can't notify waiters until after we've finished draining or they'll just requeue.
	 * Fortunately if there were waiters, we can't have been freed yet.
	 */
	if (notify)
		vdo_notify_all_waiters(&scrubber->waiters, NULL, NULL);
}

static void scrub_next_slab(struct slab_scrubber *scrubber);

/**
 * slab_scrubbed() - Notify the scrubber that a slab has been scrubbed.
 * @completion: The slab rebuild completion.
 *
 * This callback is registered in apply_journal_entries().
 */
static void slab_scrubbed(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber =
		container_of(as_vio(completion), struct slab_scrubber, vio);
	struct vdo_slab *slab = scrubber->slab;

	slab->status = VDO_SLAB_REBUILT;
	vdo_queue_slab(slab);
	vdo_reopen_slab_journal(slab->journal);
	WRITE_ONCE(scrubber->slab_count, scrubber->slab_count - 1);
	scrub_next_slab(scrubber);
}

/**
 * abort_scrubbing() - Abort scrubbing due to an error.
 * @scrubber: The slab scrubber.
 * @result: The error.
 */
static void abort_scrubbing(struct slab_scrubber *scrubber, int result)
{
	vdo_enter_read_only_mode(scrubber->vio.completion.vdo, result);
	finish_scrubbing(scrubber, result);
}

/**
 * handle_scrubber_error() - Handle errors while rebuilding a slab.
 * @completion: The slab rebuild completion.
 */
static void handle_scrubber_error(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);

	record_metadata_io_error(vio);
	abort_scrubbing(container_of(vio, struct slab_scrubber, vio), completion->result);
}

/**
 * apply_block_entries() - Apply all the entries in a block to the reference counts.
 * @block: A block with entries to apply.
 * @entry_count: The number of entries to apply.
 * @block_number: The sequence number of the block.
 * @slab: The slab to apply the entries to.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int apply_block_entries(struct packed_slab_journal_block *block,
			       journal_entry_count_t entry_count,
			       sequence_number_t block_number,
			       struct vdo_slab *slab)
{
	struct journal_point entry_point = {
		.sequence_number = block_number,
		.entry_count = 0,
	};
	int result;
	slab_block_number max_sbn = slab->end - slab->start;

	while (entry_point.entry_count < entry_count) {
		struct slab_journal_entry entry =
			vdo_decode_slab_journal_entry(block, entry_point.entry_count);

		if (entry.sbn > max_sbn)
			/* This entry is out of bounds. */
			return uds_log_error_strerror(VDO_CORRUPT_JOURNAL,
						      "vdo_slab journal entry (%llu, %u) had invalid offset %u in slab (size %u blocks)",
						      (unsigned long long) block_number,
						      entry_point.entry_count,
						      entry.sbn,
						      max_sbn);

		result = vdo_replay_reference_count_change(slab->reference_counts,
							   &entry_point,
							   entry);
		if (result != VDO_SUCCESS) {
			uds_log_error_strerror(result,
					       "vdo_slab journal entry (%llu, %u) (%s of offset %u) could not be applied in slab %u",
					       (unsigned long long) block_number,
					       entry_point.entry_count,
					       vdo_get_journal_operation_name(entry.operation),
					       entry.sbn,
					       slab->slab_number);
			return result;
		}
		entry_point.entry_count++;
	}

	return VDO_SUCCESS;
}

/**
 * apply_journal_entries() - Find the relevant vio of the slab journal and apply all valid entries.
 * @completion: The metadata read vio completion.
 *
 * This is a callback registered in start_scrubbing().
 */
static void apply_journal_entries(struct vdo_completion *completion)
{
	int result;
	struct slab_scrubber *scrubber
		= container_of(as_vio(completion), struct slab_scrubber, vio);
	struct vdo_slab *slab = scrubber->slab;
	struct slab_journal *journal = slab->journal;
	struct ref_counts *reference_counts = slab->reference_counts;

	/* Find the boundaries of the useful part of the journal. */
	sequence_number_t tail = journal->tail;
	tail_block_offset_t end_index = vdo_get_slab_journal_block_offset(journal, tail - 1);
	char *end_data = scrubber->vio.data + (end_index * VDO_BLOCK_SIZE);
	struct packed_slab_journal_block *end_block =
		(struct packed_slab_journal_block *) end_data;

	sequence_number_t head = __le64_to_cpu(end_block->header.head);
	tail_block_offset_t head_index = vdo_get_slab_journal_block_offset(journal, head);
	block_count_t index = head_index;

	struct journal_point ref_counts_point = reference_counts->slab_journal_point;
	struct journal_point last_entry_applied = ref_counts_point;
	sequence_number_t sequence;

	for (sequence = head; sequence < tail; sequence++) {
		char *block_data = scrubber->vio.data + (index * VDO_BLOCK_SIZE);
		struct packed_slab_journal_block *block =
			(struct packed_slab_journal_block *) block_data;
		struct slab_journal_block_header header;

		vdo_unpack_slab_journal_block_header(&block->header, &header);

		if ((header.nonce != slab->allocator->nonce) ||
		    (header.metadata_type != VDO_METADATA_SLAB_JOURNAL) ||
		    (header.sequence_number != sequence) ||
		    (header.entry_count > journal->entries_per_block) ||
		    (header.has_block_map_increments &&
		     (header.entry_count > journal->full_entries_per_block))) {
			/* The block is not what we expect it to be. */
			uds_log_error("vdo_slab journal block for slab %u was invalid",
				      slab->slab_number);
			abort_scrubbing(scrubber, VDO_CORRUPT_JOURNAL);
			return;
		}

		result = apply_block_entries(block, header.entry_count, sequence, slab);
		if (result != VDO_SUCCESS) {
			abort_scrubbing(scrubber, result);
			return;
		}

		last_entry_applied.sequence_number = sequence;
		last_entry_applied.entry_count = header.entry_count - 1;
		index++;
		if (index == journal->size)
			index = 0;
	}

	/*
	 * At the end of rebuild, the ref_counts should be accurate to the end of the journal we
	 * just applied.
	 */
	result = ASSERT(!vdo_before_journal_point(&last_entry_applied, &ref_counts_point),
			"Refcounts are not more accurate than the slab journal");
	if (result != VDO_SUCCESS) {
		abort_scrubbing(scrubber, result);
		return;
	}

	/* Save out the rebuilt reference blocks. */
	vdo_prepare_completion(completion,
			       slab_scrubbed,
			       handle_scrubber_error,
			       slab->allocator->thread_id,
			       completion->parent);
	vdo_start_operation_with_waiter(&slab->state,
					VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING,
					completion,
					initiate_slab_action);
}

static void read_slab_journal_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct slab_scrubber *scrubber = container_of(vio, struct slab_scrubber, vio);

	continue_vio_after_io(bio->bi_private,
			      apply_journal_entries,
			      scrubber->slab->allocator->thread_id);
}

/**
 * start_scrubbing() - Read the current slab's journal from disk now that it has been flushed.
 * @completion: The scrubber's vio completion.
 *
 * This callback is registered in scrub_next_slab().
 */
static void start_scrubbing(struct vdo_completion *completion)
{
	struct slab_scrubber *scrubber =
		container_of(as_vio(completion), struct slab_scrubber, vio);
	struct vdo_slab *slab = scrubber->slab;

	if (!slab->allocator->summary_entries[slab->slab_number].is_dirty) {
		slab_scrubbed(completion);
		return;
	}

	submit_metadata_vio(&scrubber->vio,
			    slab->journal_origin,
			    read_slab_journal_endio,
			    handle_scrubber_error,
			    REQ_OP_READ);
}

/**
 * scrub_next_slab() - Scrub the next slab if there is one.
 * @scrubber: The scrubber.
 */
static void scrub_next_slab(struct slab_scrubber *scrubber)
{
	struct vdo_completion *completion = &scrubber->vio.completion;
	struct vdo_slab *slab;

	/*
	 * Note: this notify call is always safe only because scrubbing can only be started when
	 * the VDO is quiescent.
	 */
	vdo_notify_all_waiters(&scrubber->waiters, NULL, NULL);

	if (vdo_is_read_only(completion->vdo)) {
		finish_scrubbing(scrubber, VDO_READ_ONLY);
		return;
	}

	slab = get_next_slab(scrubber);
	if ((slab == NULL) ||
	    (scrubber->high_priority_only && list_empty(&scrubber->high_priority_slabs))) {
		finish_scrubbing(scrubber, VDO_SUCCESS);
		return;
	}

	if (vdo_finish_draining(&scrubber->admin_state))
		return;

	list_del_init(&slab->allocq_entry);
	scrubber->slab = slab;
	vdo_prepare_completion(completion,
			       start_scrubbing,
			       handle_scrubber_error,
			       slab->allocator->thread_id,
			       completion->parent);
	vdo_start_operation_with_waiter(&slab->state,
					VDO_ADMIN_STATE_SCRUBBING,
					completion,
					initiate_slab_action);
}

/**
 * scrub_slabs() - Scrub all of an allocator's slabs that are eligible for scrubbing.
 * @allocator: The block_allocator to scrub.
 * @parent: The completion to notify when scrubbing is done, implies high_priority, may be NULL.
 */
EXTERNAL_STATIC void scrub_slabs(struct block_allocator *allocator, struct vdo_completion *parent)
{
	struct slab_scrubber *scrubber = &allocator->scrubber;

	scrubber->vio.completion.parent = parent;
	scrubber->high_priority_only = (parent != NULL);
	if (!has_slabs_to_scrub(scrubber)) {
		finish_scrubbing(scrubber, VDO_SUCCESS);
		return;
	}

	if (scrubber->high_priority_only &&
	    is_priority_table_empty(allocator->prioritized_slabs) &&
	    list_empty(&scrubber->high_priority_slabs))
		vdo_register_slab_for_scrubbing(get_next_slab(scrubber), true);

	vdo_resume_if_quiescent(&scrubber->admin_state);
	scrub_next_slab(scrubber);
}

/* FULLNESS HINT COMPUTATION */

/**
 * compute_fullness_hint() - Translate a slab's free block count into a 'fullness hint' that can be
 *                           stored in a slab_summary_entry's 7 bits that are dedicated to its free
 *                           count.
 * @depot: The depot whose summary being updated.
 * @free_blocks: The number of free blocks.
 *
 * Note: the number of free blocks must be strictly less than 2^23 blocks, even though
 * theoretically slabs could contain precisely 2^23 blocks; there is an assumption that at least
 * one block is used by metadata. This assumption is necessary; otherwise, the fullness hint might
 * overflow. The fullness hint formula is roughly (fullness >> 16) & 0x7f, but ((1 > 16) & 0x7f is
 * the same as (0 >> 16) & 0x7f, namely 0, which is clearly a bad hint if it could indicate both
 * 2^23 free blocks or 0 free blocks.
 *
 * Return: A fullness hint, which can be stored in 7 bits.
 */
static u8 __must_check compute_fullness_hint(struct slab_depot *depot, block_count_t free_blocks)
{
	block_count_t hint;

	ASSERT_LOG_ONLY((free_blocks < (1 << 23)), "free blocks must be less than 2^23");

	if (free_blocks == 0)
		return 0;

	hint = free_blocks >> depot->hint_shift;
	return ((hint == 0) ? 1 : hint);
}

/**
 * check_summary_drain_complete() - Check whether an allocators summary has finished draining.
 */
static void check_summary_drain_complete(struct block_allocator *allocator)
{
	struct vdo *vdo = allocator->depot->vdo;

	if (!vdo_is_state_draining(&allocator->summary_state) ||
	    (allocator->summary_write_count > 0))
		return;

	vdo_finish_operation(&allocator->summary_state,
			     (vdo_is_read_only(vdo) ? VDO_READ_ONLY : VDO_SUCCESS));
}

/**
 * notify_summary_waiters() - Wake all the waiters in a given queue.
 * @allocator: The block allocator summary which owns the queue.
 * @queue: The queue to notify.
 */
static void notify_summary_waiters(struct block_allocator *allocator, struct wait_queue *queue)
{
	int result = (vdo_is_read_only(allocator->depot->vdo) ? VDO_READ_ONLY : VDO_SUCCESS);

	vdo_notify_all_waiters(queue, NULL, &result);
}

static void launch_write(struct slab_summary_block *summary_block);

/**
 * finish_updating_slab_summary_block() - Finish processing a block which attempted to write,
 *                                        whether or not the attempt succeeded.
 * @block: The block.
 */
static void finish_updating_slab_summary_block(struct slab_summary_block *block)
{
	notify_summary_waiters(block->allocator, &block->current_update_waiters);
	block->writing = false;
	block->allocator->summary_write_count--;
	if (vdo_has_waiters(&block->next_update_waiters))
		launch_write(block);
	else
		check_summary_drain_complete(block->allocator);
}

/**
 * finish_update() - This is the callback for a successful summary block write.
 * @completion: The write vio.
 */
static void finish_update(struct vdo_completion *completion)
{
	struct slab_summary_block *block =
		container_of(as_vio(completion), struct slab_summary_block, vio);

	atomic64_inc(&block->allocator->depot->summary_statistics.blocks_written);
	finish_updating_slab_summary_block(block);
}

/**
 * handle_write_error() - Handle an error writing a slab summary block.
 * @completion: The write VIO.
 */
static void handle_write_error(struct vdo_completion *completion)
{
	struct slab_summary_block *block =
		container_of(as_vio(completion), struct slab_summary_block, vio);

	record_metadata_io_error(as_vio(completion));
	vdo_enter_read_only_mode(completion->vdo, completion->result);
	finish_updating_slab_summary_block(block);
}

static void write_slab_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct slab_summary_block *block = container_of(vio, struct slab_summary_block, vio);

	continue_vio_after_io(vio, finish_update, block->allocator->thread_id);
}

/**
 * launch_write() - Write a slab summary block unless it is currently out for writing.
 * @block: The block that needs to be committed.
 */
static void launch_write(struct slab_summary_block *block)
{
	struct block_allocator *allocator = block->allocator;
	struct slab_depot *depot = allocator->depot;
	physical_block_number_t pbn;

	if (block->writing)
		return;

	allocator->summary_write_count++;
	vdo_transfer_all_waiters(&block->next_update_waiters, &block->current_update_waiters);
	block->writing = true;

	if (vdo_is_read_only(depot->vdo)) {
		finish_updating_slab_summary_block(block);
		return;
	}

	memcpy(block->outgoing_entries, block->entries, VDO_BLOCK_SIZE);

	/*
	 * Flush before writing to ensure that the slab journal tail blocks and reference updates
	 * covered by this summary update are stable (VDO-2332).
	 */
	pbn = (depot->summary_origin +
	       (VDO_SLAB_SUMMARY_BLOCKS_PER_ZONE * allocator->zone_number) +
	       block->index);
	submit_metadata_vio(&block->vio,
			    pbn,
			    write_slab_summary_endio,
			    handle_write_error,
			    REQ_OP_WRITE | REQ_PREFLUSH);
}

/**
 * vdo_update_slab_summary_entry() - Update the entry for a slab.
 * @slab: The slab whose entry is to be updated
 * @waiter: The waiter that is updating the summary.
 * @tail_block_offset: The offset of the slab journal's tail block.
 * @load_ref_counts: Whether the reference counts must be loaded from disk on the vdo load.
 * @is_clean: Whether the slab is clean.
 * @free_blocks: The number of free blocks.
 */
void vdo_update_slab_summary_entry(struct vdo_slab *slab,
				   struct waiter *waiter,
				   tail_block_offset_t tail_block_offset,
				   bool load_ref_counts,
				   bool is_clean,
				   block_count_t free_blocks)
{
	u8 index = slab->slab_number / VDO_SLAB_SUMMARY_ENTRIES_PER_BLOCK;
	struct block_allocator *allocator = slab->allocator;
	struct slab_summary_block *block = &allocator->summary_blocks[index];
	int result;
	struct slab_summary_entry *entry;

	if (vdo_is_read_only(block->vio.completion.vdo)) {
		result = VDO_READ_ONLY;
		waiter->callback(waiter, &result);
		return;
	}

	if (vdo_is_state_draining(&allocator->summary_state) ||
	    vdo_is_state_quiescent(&allocator->summary_state)) {
		result = VDO_INVALID_ADMIN_STATE;
		waiter->callback(waiter, &result);
		return;
	}

	entry = &allocator->summary_entries[slab->slab_number];
	*entry = (struct slab_summary_entry) {
		.tail_block_offset = tail_block_offset,
		.load_ref_counts = (entry->load_ref_counts || load_ref_counts),
		.is_dirty = !is_clean,
		.fullness_hint = compute_fullness_hint(allocator->depot, free_blocks),
	};
	vdo_enqueue_waiter(&block->next_update_waiters, waiter);
	launch_write(block);
}

/**
 * vdo_set_slab_summary_origin() - Set the origin of the slab summary relative to the physical
 *                                 layer.
 * @summary: The slab_summary to update.
 * @partition: The slab summary partition.
 */
void vdo_set_slab_summary_origin(struct slab_depot *depot, struct partition *partition)
{
	depot->summary_origin = vdo_get_fixed_layout_partition_offset(partition);
}

static inline void assert_on_allocator_thread(thread_id_t thread_id, const char *function_name)
{
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() == thread_id),
			"%s called on correct thread",
			function_name);
}

/*
 * Slabs are essentially prioritized by an approximation of the number of free blocks in the slab
 * so slabs with lots of free blocks with be opened for allocation before slabs that have few free
 * blocks.
 */
static unsigned int calculate_slab_priority(struct vdo_slab *slab)
{
	block_count_t free_blocks = slab->reference_counts->free_blocks;
	unsigned int unopened_slab_priority = slab->allocator->unopened_slab_priority;
	unsigned int priority;

	/*
	 * Wholly full slabs must be the only ones with lowest priority, 0.
	 *
	 * Slabs that have never been opened (empty, newly initialized, and never been written to)
	 * have lower priority than previously opened slabs that have a significant number of free
	 * blocks. This ranking causes VDO to avoid writing physical blocks for the first time
	 * unless there are very few free blocks that have been previously written to.
	 *
	 * Since VDO doesn't discard blocks currently, reusing previously written blocks makes VDO
	 * a better client of any underlying storage that is thinly-provisioned (though discarding
	 * would be better).
	 *
	 * For all other slabs, the priority is derived from the logarithm of the number of free
	 * blocks. Slabs with the same order of magnitude of free blocks have the same priority.
	 * With 2^23 blocks, the priority will range from 1 to 25. The reserved
	 * unopened_slab_priority divides the range and is skipped by the logarithmic mapping.
	 */

	if (free_blocks == 0)
		return 0;

	if (vdo_is_slab_journal_blank(slab->journal))
		return unopened_slab_priority;

	priority = (1 + ilog2(free_blocks));
	return ((priority < unopened_slab_priority) ? priority : priority + 1);
}

static void prioritize_slab(struct vdo_slab *slab)
{
	ASSERT_LOG_ONLY(list_empty(&slab->allocq_entry),
			"a slab must not already be on a ring when prioritizing");
	slab->priority = calculate_slab_priority(slab);
	priority_table_enqueue(slab->allocator->prioritized_slabs,
			       slab->priority,
			       &slab->allocq_entry);
}

static void register_slab_with_allocator(struct block_allocator *allocator, struct vdo_slab *slab)
{
	allocator->slab_count++;
	allocator->last_slab = slab->slab_number;
}

/**
 * get_depot_slab_iterator() - Return a slab_iterator over the slabs in a slab_depot.
 * @depot: The depot over which to iterate.
 * @start: The number of the slab to start iterating from.
 * @end: The number of the last slab which may be returned.
 * @stride: The difference in slab number between successive slabs.
 *
 * Iteration always occurs from higher to lower numbered slabs.
 *
 * Return: An initialized iterator structure.
 */
static struct slab_iterator get_depot_slab_iterator(struct slab_depot *depot,
						    slab_count_t start,
						    slab_count_t end,
						    slab_count_t stride)
{
	struct vdo_slab **slabs = depot->slabs;

	return (struct slab_iterator) {
		.slabs = slabs,
		.next = (((slabs == NULL) || (start < end)) ? NULL : slabs[start]),
		.end = end,
		.stride = stride,
	};
}

static struct slab_iterator get_slab_iterator(const struct block_allocator *allocator)
{
	return get_depot_slab_iterator(allocator->depot,
				       allocator->last_slab,
				       allocator->zone_number,
				       allocator->depot->zone_count);
}

/**
 * next_slab() - Get the next slab from a slab_iterator and advance the iterator
 * @iterator: The slab_iterator.
 *
 * Return: The next slab or NULL if the iterator is exhausted.
 */
static struct vdo_slab *next_slab(struct slab_iterator *iterator)
{
	struct vdo_slab *slab = iterator->next;

	if ((slab == NULL) || (slab->slab_number < iterator->end + iterator->stride))
		iterator->next = NULL;
	else
		iterator->next = iterator->slabs[slab->slab_number - iterator->stride];

	return slab;
}

/* Implements vdo_read_only_notification. */
static void notify_block_allocator_of_read_only_mode(void *listener, struct vdo_completion *parent)
{
	struct block_allocator *allocator = listener;
	struct slab_iterator iterator;

	assert_on_allocator_thread(allocator->thread_id, __func__);
	iterator = get_slab_iterator(allocator);
	while (iterator.next != NULL)
		vdo_abort_slab_journal_waiters(next_slab(&iterator)->journal);

	vdo_finish_completion(parent);
}


/* Queue a slab for allocation or scrubbing. */
void vdo_queue_slab(struct vdo_slab *slab)
{
	struct block_allocator *allocator = slab->allocator;
	block_count_t free_blocks;
	int result;

	ASSERT_LOG_ONLY(list_empty(&slab->allocq_entry),
			"a requeued slab must not already be on a ring");
	free_blocks = slab->reference_counts->free_blocks;
	result = ASSERT((free_blocks <= allocator->depot->slab_config.data_blocks),
			"rebuilt slab %u must have a valid free block count (has %llu, expected maximum %llu)",
			slab->slab_number,
			(unsigned long long) free_blocks,
			(unsigned long long) allocator->depot->slab_config.data_blocks);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(allocator->depot->vdo, result);
		return;
	}

	if (slab->status != VDO_SLAB_REBUILT) {
		vdo_register_slab_for_scrubbing(slab, false);
		return;
	}

	if (!vdo_is_state_resuming(&slab->state)) {
		/*
		 * If the slab is resuming, we've already accounted for it here, so don't do it
		 * again.
		 * FIXME: under what situation would the slab be resuming here?
		 */
		WRITE_ONCE(allocator->allocated_blocks, allocator->allocated_blocks - free_blocks);
		if (!vdo_is_slab_journal_blank(slab->journal))
			WRITE_ONCE(allocator->statistics.slabs_opened,
				   allocator->statistics.slabs_opened + 1);
	}

	vdo_resume_slab_journal(slab->journal);
	prioritize_slab(slab);
}

/**
 * vdo_adjust_free_block_count() - Adjust the free block count and (if needed) reprioritize the
 *                                 slab.
 * @increment: should be true if the free block count went up.
 */
void vdo_adjust_free_block_count(struct vdo_slab *slab, bool increment)
{
	struct block_allocator *allocator = slab->allocator;

	WRITE_ONCE(allocator->allocated_blocks,
		   allocator->allocated_blocks + (increment ? -1 : 1));

	/* The open slab doesn't need to be reprioritized until it is closed. */
	if (slab == allocator->open_slab)
		return;

	/* Don't bother adjusting the priority table if unneeded. */
	if (slab->priority == calculate_slab_priority(slab))
		return;

	/*
	 * Reprioritize the slab to reflect the new free block count by removing it from the table
	 * and re-enqueuing it with the new priority.
	 */
	priority_table_remove(allocator->prioritized_slabs, &slab->allocq_entry);
	prioritize_slab(slab);
}

/**
 * vdo_acquire_provisional_reference() - Acquire a provisional reference on behalf of a PBN lock if
 *                                       the block it locks is unreferenced.
 * @slab: The slab which contains the block.
 * @pbn: The physical block to reference.
 * @lock: The lock.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_acquire_provisional_reference(struct vdo_slab *slab,
				      physical_block_number_t pbn,
				      struct pbn_lock *lock)
{
	int result;

	if (vdo_pbn_lock_has_provisional_reference(lock))
		return VDO_SUCCESS;

	result = vdo_provisionally_reference_block(slab->reference_counts, pbn, lock);
	if (result != VDO_SUCCESS)
		return result;

	if (vdo_pbn_lock_has_provisional_reference(lock))
		vdo_adjust_free_block_count(slab, false);

	return VDO_SUCCESS;
}

static int allocate_slab_block(struct vdo_slab *slab, physical_block_number_t *block_number_ptr)
{
	physical_block_number_t pbn;
	int result;

	result = vdo_allocate_unreferenced_block(slab->reference_counts, &pbn);
	if (result != VDO_SUCCESS)
		return result;

	vdo_adjust_free_block_count(slab, false);

	*block_number_ptr = pbn;
	return VDO_SUCCESS;
}

/**
 * open_slab() - Prepare a slab to be allocated from.
 * @slab: The slab.
 */
static void open_slab(struct vdo_slab *slab)
{
	vdo_reset_search_cursor(slab->reference_counts);
	if (vdo_is_slab_journal_blank(slab->journal)) {
		WRITE_ONCE(slab->allocator->statistics.slabs_opened,
			   slab->allocator->statistics.slabs_opened + 1);
		vdo_dirty_all_reference_blocks(slab->reference_counts);
	} else {
		WRITE_ONCE(slab->allocator->statistics.slabs_reopened,
			   slab->allocator->statistics.slabs_reopened + 1);
	}

	slab->allocator->open_slab = slab;
}


/*
 * The block allocated will have a provisional reference and the reference must be either confirmed
 * with a subsequent increment or vacated with a subsequent decrement via
 * vdo_release_block_reference().
 */
int vdo_allocate_block(struct block_allocator *allocator,
		       physical_block_number_t *block_number_ptr)
{
	int result;

	if (allocator->open_slab != NULL) {
		/* Try to allocate the next block in the currently open slab. */
		result = allocate_slab_block(allocator->open_slab, block_number_ptr);
		if ((result == VDO_SUCCESS) || (result != VDO_NO_SPACE))
			return result;

		/* Put the exhausted open slab back into the priority table. */
		prioritize_slab(allocator->open_slab);
	}

	/* Remove the highest priority slab from the priority table and make it the open slab. */
	open_slab(list_entry(priority_table_dequeue(allocator->prioritized_slabs),
			     struct vdo_slab,
			     allocq_entry));

	/*
	 * Try allocating again. If we're out of space immediately after opening a slab, then every
	 * slab must be fully allocated.
	 */
	return allocate_slab_block(allocator->open_slab, block_number_ptr);
}

/**
 * vdo_enqueue_clean_slab_waiter() - Wait for a clean slab.
 * @allocator: The block_allocator on which to wait.
 * @waiter: The waiter.
 *
 * Return: VDO_SUCCESS if the waiter was queued, VDO_NO_SPACE if there are no slabs to scrub, and
 *         some other error otherwise.
 */
int vdo_enqueue_clean_slab_waiter(struct block_allocator *allocator, struct waiter *waiter)
{
	if (vdo_is_read_only(allocator->depot->vdo))
		return VDO_READ_ONLY;

	if (vdo_is_state_quiescent(&allocator->scrubber.admin_state))
		return VDO_NO_SPACE;

	vdo_enqueue_waiter(&allocator->scrubber.waiters, waiter);
	return VDO_SUCCESS;
}

/**
 * vdo_modify_slab_reference_count() - Increment or decrement the reference count of a block in a
 *                                     slab.
 * @slab: The slab containing the block (may be NULL when referencing the zero block).
 * @journal_point: The slab journal entry corresponding to this change.
 * @updater: The reference count updater.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_modify_slab_reference_count(struct vdo_slab *slab,
				    const struct journal_point *journal_point,
				    struct reference_updater *updater)
{
	bool free_status_changed;
	int result;

	if (slab == NULL)
		return VDO_SUCCESS;

	/*
	 * If the slab is unrecovered, preserve the refCount state and let scrubbing correct the
	 * refCount. Note that the slab journal has already captured all refCount updates.
	 */
	if (slab->status != VDO_SLAB_REBUILT) {
		vdo_adjust_slab_journal_block_reference(slab->journal,
							journal_point->sequence_number,
							-1);
		return VDO_SUCCESS;
	}

	result = vdo_adjust_reference_count(slab->reference_counts,
					    updater,
					    journal_point,
					    &free_status_changed);
	if (result != VDO_SUCCESS)
		return result;

	if (free_status_changed)
		vdo_adjust_free_block_count(slab, !updater->increment);

	return VDO_SUCCESS;
}

/* Release an unused provisional reference. */
void vdo_release_block_reference(struct block_allocator *allocator,
				 physical_block_number_t pbn,
				 const char *why)
{
	int result;
	struct reference_updater updater;

	if (pbn == VDO_ZERO_BLOCK)
		return;

	updater = (struct reference_updater) {
		.operation = VDO_JOURNAL_DATA_REMAPPING,
		.increment = false,
		.zpbn = {
			.pbn = pbn,
		},
	};

	result = vdo_modify_slab_reference_count(vdo_get_slab(allocator->depot, pbn),
						 NULL,
						 &updater);
	if (result != VDO_SUCCESS)
		uds_log_error_strerror(result,
				       "Failed to release reference to %s physical block %llu",
				       why,
				       (unsigned long long) pbn);
}

/**
 * compare_slab_statuses() - This is a heap_comparator function that orders slab_status structures
 *                           using the 'is_clean' field as the primary key and the 'emptiness'
 *                           field as the secondary key.
 * @item1: The first item to compare.
 * @item2: The second item to compare.
 *
 * Slabs need to be pushed onto the rings in the same order they are to be popped off. Popping
 * should always get the most empty first, so pushing should be from most empty to least empty.
 * Thus, the comparator order is the usual sense since the heap structure returns larger elements
 * before smaller ones.
 *
 * Return:  1 if the first item is cleaner or emptier than the second;
 *          0 if the two items are equally clean and empty;
 *	   -1 otherwise
 */
static int compare_slab_statuses(const void *item1, const void *item2)
{
	const struct slab_status *info1 = (const struct slab_status *) item1;
	const struct slab_status *info2 = (const struct slab_status *) item2;

	if (info1->is_clean != info2->is_clean)
		return info1->is_clean ? 1 : -1;
	if (info1->emptiness != info2->emptiness)
		return ((info1->emptiness > info2->emptiness) ? 1 : -1);
	return (info1->slab_number < info2->slab_number) ? 1 : -1;
}

/* Implements heap_swapper. */
static void swap_slab_statuses(void *item1, void *item2)
{
	struct slab_status *info1 = item1;
	struct slab_status *info2 = item2;
	struct slab_status temp = *info1;

	*info1 = *info2;
	*info2 = temp;
}

/* Inform the slab actor that a action has finished on some slab; used by apply_to_slabs(). */
static void slab_action_callback(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);
	struct slab_actor *actor = &allocator->slab_actor;

	if (--actor->slab_action_count == 0) {
		actor->callback(completion);
		return;
	}

	vdo_reset_completion(completion);
}

/* Preserve the error from part of an action and continue. */
static void handle_operation_error(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);

	if (allocator->state.waiter != NULL)
		vdo_set_completion_result(allocator->state.waiter, completion->result);
	completion->callback(completion);
}

/* Perform an action on each of an allocator's slabs in parallel. */
static void apply_to_slabs(struct block_allocator *allocator, vdo_action *callback)
{
	struct slab_iterator iterator;

	vdo_prepare_completion(&allocator->completion,
			       slab_action_callback,
			       handle_operation_error,
			       allocator->thread_id,
			       NULL);
	allocator->completion.requeue = false;

	/*
	 * Since we are going to dequeue all of the slabs, the open slab will become invalid, so
	 * clear it.
	 */
	allocator->open_slab = NULL;

	/* Ensure that we don't finish before we're done starting. */
	allocator->slab_actor = (struct slab_actor) {
		.slab_action_count = 1,
		.callback = callback,
	};

	iterator = get_slab_iterator(allocator);
	while (iterator.next != NULL) {
		const struct admin_state_code *operation =
			vdo_get_admin_state_code(&allocator->state);
		struct vdo_slab *slab = next_slab(&iterator);

		list_del_init(&slab->allocq_entry);
		allocator->slab_actor.slab_action_count++;
		vdo_start_operation_with_waiter(&slab->state,
						operation,
						&allocator->completion,
						initiate_slab_action);
	}

	slab_action_callback(&allocator->completion);
}

static void finish_loading_allocator(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);
	const struct admin_state_code *operation = vdo_get_admin_state_code(&allocator->state);

	if (allocator->eraser != NULL)
		dm_kcopyd_client_destroy(UDS_FORGET(allocator->eraser));

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_RECOVERY) {
		void *context = vdo_get_current_action_context(allocator->depot->action_manager);

		vdo_replay_into_slab_journals(allocator, context);
		return;
	}

	vdo_finish_loading(&allocator->state);
}

static void erase_next_slab_journal(struct block_allocator *allocator);

static void copy_callback(int read_err, unsigned long write_err, void *context)
{
	struct block_allocator *allocator = context;
	int result = (((read_err == 0) && (write_err == 0)) ? VDO_SUCCESS : -EIO);

	if (result != VDO_SUCCESS) {
		vdo_fail_completion(&allocator->completion, result);
		return;
	}

	erase_next_slab_journal(allocator);
}

/* erase_next_slab_journal() - Erase the next slab journal. */
static void erase_next_slab_journal(struct block_allocator *allocator)
{
	struct vdo_slab *slab;
	physical_block_number_t pbn;
	struct dm_io_region regions[1];
	struct slab_depot *depot = allocator->depot;
	block_count_t blocks = depot->slab_config.slab_journal_blocks;

	if (allocator->slabs_to_erase.next == NULL) {
		vdo_finish_completion(&allocator->completion);
		return;
	}

	slab = next_slab(&allocator->slabs_to_erase);
	pbn = slab->journal_origin - depot->vdo->geometry.bio_offset;
	regions[0] = (struct dm_io_region) {
		.bdev = vdo_get_backing_device(depot->vdo),
		.sector = pbn * VDO_SECTORS_PER_BLOCK,
		.count = blocks * VDO_SECTORS_PER_BLOCK,
	};
	dm_kcopyd_zero(allocator->eraser, 1, regions, 0, copy_callback, allocator);
}

/* Implements vdo_admin_initiator. */
static void initiate_load(struct admin_state *state)
{
	struct block_allocator *allocator = container_of(state, struct block_allocator, state);
	const struct admin_state_code *operation = vdo_get_admin_state_code(state);

	if (operation == VDO_ADMIN_STATE_LOADING_FOR_REBUILD) {
		/*
		 * Must requeue because the kcopyd client cannot be freed in the same stack frame
		 * as the kcopyd callback, lest it deadlock.
		 */
		vdo_prepare_completion_for_requeue(&allocator->completion,
						   finish_loading_allocator,
						   handle_operation_error,
						   allocator->thread_id,
						   NULL);
		allocator->eraser = dm_kcopyd_client_create(NULL);
		if (allocator->eraser == NULL) {
			vdo_fail_completion(&allocator->completion, -ENOMEM);
			return;
		}
		allocator->slabs_to_erase = get_slab_iterator(allocator);

		erase_next_slab_journal(allocator);
		return;
	}

	apply_to_slabs(allocator, finish_loading_allocator);
}

/*
 * vdo_notify_slab_journals_are_recovered(): Inform a block allocator that its slab journals have
 *                                           been recovered from the recovery journal.
 * @completion The allocator completion
 */
void vdo_notify_slab_journals_are_recovered(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);

	vdo_finish_loading_with_result(&allocator->state, completion->result);
}

EXTERNAL_STATIC int get_slab_statuses(struct block_allocator *allocator,
				      struct slab_status **statuses_ptr)
{
	int result;
	struct slab_status *statuses;
	struct slab_iterator iterator = get_slab_iterator(allocator);

	result = UDS_ALLOCATE(allocator->slab_count, struct slab_status, __func__, &statuses);
	if (result != VDO_SUCCESS)
		return result;

	*statuses_ptr = statuses;

	while (iterator.next != NULL)  {
		slab_count_t slab_number = next_slab(&iterator)->slab_number;

		*statuses++ = (struct slab_status) {
			.slab_number = slab_number,
			.is_clean = !allocator->summary_entries[slab_number].is_dirty,
			.emptiness = allocator->summary_entries[slab_number].fullness_hint,
		};
	}

	return VDO_SUCCESS;
}

/* Prepare slabs for allocation or scrubbing. */
EXTERNAL_STATIC int __must_check
vdo_prepare_slabs_for_allocation(struct block_allocator *allocator)
{
	struct slab_status current_slab_status;
	struct heap heap;
	int result;
	struct slab_status *slab_statuses;
	struct slab_depot *depot = allocator->depot;

	WRITE_ONCE(allocator->allocated_blocks,
		   allocator->slab_count * depot->slab_config.data_blocks);
	result = get_slab_statuses(allocator, &slab_statuses);
	if (result != VDO_SUCCESS)
		return result;

	/* Sort the slabs by cleanliness, then by emptiness hint. */
	initialize_heap(&heap,
			compare_slab_statuses,
			swap_slab_statuses,
			slab_statuses,
			allocator->slab_count,
			sizeof(struct slab_status));
	build_heap(&heap, allocator->slab_count);

	while (pop_max_heap_element(&heap, &current_slab_status)) {
		bool high_priority;
		struct vdo_slab *slab = depot->slabs[current_slab_status.slab_number];

		if ((depot->load_type == VDO_SLAB_DEPOT_REBUILD_LOAD) ||
		    (!allocator->summary_entries[slab->slab_number].load_ref_counts &&
		     current_slab_status.is_clean)) {
			vdo_queue_slab(slab);
			continue;
		}

		slab->status = VDO_SLAB_REQUIRES_SCRUBBING;
		high_priority = ((current_slab_status.is_clean &&
				 (depot->load_type == VDO_SLAB_DEPOT_NORMAL_LOAD)) ||
				 vdo_slab_journal_requires_scrubbing(slab->journal));
		vdo_register_slab_for_scrubbing(slab, high_priority);
	}

	UDS_FREE(slab_statuses);
	return VDO_SUCCESS;
}

#ifdef INTERNAL
void vdo_allocate_from_allocator_last_slab(struct block_allocator *allocator)
{
	struct vdo_slab *last_slab = allocator->depot->slabs[allocator->last_slab];

	ASSERT_LOG_ONLY(allocator->open_slab == NULL, "mustn't have an open slab");
	priority_table_remove(allocator->prioritized_slabs, &last_slab->allocq_entry);
	open_slab(last_slab);
}

#endif /* INTERNAL */
static const char *status_to_string(enum slab_rebuild_status status)
{
	switch (status) {
	case VDO_SLAB_REBUILT:
		return "REBUILT";
	case VDO_SLAB_REQUIRES_SCRUBBING:
		return "SCRUBBING";
	case VDO_SLAB_REQUIRES_HIGH_PRIORITY_SCRUBBING:
		return "PRIORITY_SCRUBBING";
	case VDO_SLAB_REBUILDING:
		return "REBUILDING";
	case VDO_SLAB_REPLAYING:
		return "REPLAYING";
	default:
		return "UNKNOWN";
	}
}

void vdo_dump_block_allocator(const struct block_allocator *allocator)
{
	unsigned int pause_counter = 0;
	struct slab_iterator iterator = get_slab_iterator(allocator);
	const struct slab_scrubber *scrubber = &allocator->scrubber;

	uds_log_info("block_allocator zone %u", allocator->zone_number);
	while (iterator.next != NULL) {
		struct vdo_slab *slab = next_slab(&iterator);

		if (slab->reference_counts != NULL)
			/* Terse because there are a lot of slabs to dump and syslog is lossy. */
			uds_log_info("slab %u: P%u, %llu free",
				     slab->slab_number,
				     slab->priority,
				     (unsigned long long) slab->reference_counts->free_blocks);
		else
			uds_log_info("slab %u: status %s",
				     slab->slab_number,
				     status_to_string(slab->status));

		vdo_dump_slab_journal(slab->journal);

		if (slab->reference_counts != NULL)
			vdo_dump_ref_counts(slab->reference_counts);
		else
			uds_log_info("refCounts is null");

		/*
		 * Wait for a while after each batch of 32 slabs dumped, an arbitrary number,
		 * allowing the kernel log a chance to be flushed instead of being overrun.
		 */
		if (pause_counter++ == 31) {
			pause_counter = 0;
			uds_pause_for_logger();
		}
	}

	uds_log_info("slab_scrubber slab_count %u waiters %zu %s%s",
		     READ_ONCE(scrubber->slab_count),
		     vdo_count_waiters(&scrubber->waiters),
		     vdo_get_admin_state_code(&scrubber->admin_state)->name,
		     scrubber->high_priority_only ? ", high_priority_only " : "");
}

/**
 * allocate_slabs() - Allocate a new slab pointer array.
 * @depot: The depot.
 * @slab_count: The number of slabs the depot should have in the new array.
 *
 * Any existing slab pointers will be copied into the new array, and slabs will be allocated as
 * needed. The newly allocated slabs will not be distributed for use by the block allocators.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int allocate_slabs(struct slab_depot *depot, slab_count_t slab_count)
{
	block_count_t slab_size;
	bool resizing = false;
	physical_block_number_t slab_origin;
	block_count_t translation;
	int result;

	result = UDS_ALLOCATE(slab_count,
			      struct vdo_slab *,
			      "slab pointer array",
			      &depot->new_slabs);
	if (result != VDO_SUCCESS)
		return result;

	if (depot->slabs != NULL) {
		memcpy(depot->new_slabs,
		       depot->slabs,
		       depot->slab_count * sizeof(struct vdo_slab *));
		resizing = true;
	}

	slab_size = depot->slab_config.slab_blocks;
	slab_origin = depot->first_block + (depot->slab_count * slab_size);

	/* The translation between allocator partition PBNs and layer PBNs. */
	translation = depot->origin - depot->first_block;
	depot->new_slab_count = depot->slab_count;
	while (depot->new_slab_count < slab_count) {
		struct block_allocator *allocator =
			&depot->allocators[depot->new_slab_count % depot->zone_count];
		struct vdo_slab **slab_ptr = &depot->new_slabs[depot->new_slab_count];

		result = vdo_make_slab(slab_origin,
				       allocator,
				       translation,
				       depot->vdo->recovery_journal,
				       depot->new_slab_count,
				       resizing,
				       slab_ptr);
		if (result != VDO_SUCCESS)
			return result;

		/* Increment here to ensure that vdo_abandon_new_slabs will clean up correctly. */
		depot->new_slab_count++;

		slab_origin += slab_size;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_abandon_new_slabs() - Abandon any new slabs in this depot, freeing them as needed.
 * @depot: The depot.
 */
void vdo_abandon_new_slabs(struct slab_depot *depot)
{
	slab_count_t i;

	if (depot->new_slabs == NULL)
		return;

	for (i = depot->slab_count; i < depot->new_slab_count; i++)
		vdo_free_slab(UDS_FORGET(depot->new_slabs[i]));
	depot->new_slab_count = 0;
	depot->new_size = 0;
	UDS_FREE(UDS_FORGET(depot->new_slabs));
}

/**
 * get_allocator_thread_id() - Get the ID of the thread on which a given allocator operates.
 *
 * Implements vdo_zone_thread_getter.
 */
static thread_id_t get_allocator_thread_id(void *context, zone_count_t zone_number)
{
	return ((struct slab_depot *) context)->allocators[zone_number].thread_id;
}

/*
 * Request a commit of all dirty tail blocks which are locking the recovery journal block the depot
 * is seeking to release.
 *
 * Implements vdo_zone_action.
 */
static void release_tail_block_locks(void *context,
				     zone_count_t zone_number,
				     struct vdo_completion *parent)
{
	struct slab_journal *journal, *tmp;
	struct slab_depot *depot = context;
	struct list_head *list = &depot->allocators[zone_number].dirty_slab_journals;

	list_for_each_entry_safe(journal, tmp, list, dirty_entry) {
		if (!vdo_release_recovery_journal_lock(journal, depot->active_release_request))
			break;
	}

	vdo_finish_completion(parent);
}

/**
 * prepare_for_tail_block_commit() - Prepare to commit oldest tail blocks.
 *
 * Implements vdo_action_preamble.
 */
static void prepare_for_tail_block_commit(void *context, struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	depot->active_release_request = depot->new_release_request;
	vdo_finish_completion(parent);
}

/**
 * schedule_tail_block_commit() - Schedule a tail block commit if necessary.
 *
 * This method should not be called directly. Rather, call vdo_schedule_default_action() on the
 * depot's action manager.
 *
 * Implements vdo_action_scheduler.
 */
static bool schedule_tail_block_commit(void *context)
{
	struct slab_depot *depot = context;

	if (depot->new_release_request == depot->active_release_request)
		return false;

	return vdo_schedule_action(depot->action_manager,
				   prepare_for_tail_block_commit,
				   release_tail_block_locks,
				   NULL,
				   NULL);
}

/**
 * initialize_slab_scrubber() - Initialize an allocator's slab scrubber.
 * @allocator: The allocator being initialized
 *
 * Return: VDO_SUCCESS or an error.
 */
EXTERNAL_STATIC int initialize_slab_scrubber(struct block_allocator *allocator)
{
	struct slab_scrubber *scrubber = &allocator->scrubber;
	block_count_t slab_journal_size = allocator->depot->slab_config.slab_journal_blocks;
	char *journal_data;
	int result;

	result = UDS_ALLOCATE(VDO_BLOCK_SIZE * slab_journal_size, char, __func__, &journal_data);
	if (result != VDO_SUCCESS)
		return result;

	result = allocate_vio_components(allocator->completion.vdo,
					 VIO_TYPE_SLAB_JOURNAL,
					 VIO_PRIORITY_METADATA,
					 allocator,
					 slab_journal_size,
					 journal_data,
					 &scrubber->vio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(journal_data);
		return result;
	}

	INIT_LIST_HEAD(&scrubber->high_priority_slabs);
	INIT_LIST_HEAD(&scrubber->slabs);
	vdo_set_admin_state_code(&scrubber->admin_state, VDO_ADMIN_STATE_SUSPENDED);
	return VDO_SUCCESS;
}

/**
 * initialize_slab_summary_block() - Initialize a slab_summary_block.
 * @allocator: The allocator which owns the block.
 * @index: The index of this block in its zone's summary.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
initialize_slab_summary_block(struct block_allocator *allocator, block_count_t index)
{
	struct slab_summary_block *block = &allocator->summary_blocks[index];
	int result;

	result = UDS_ALLOCATE(VDO_BLOCK_SIZE, char, __func__, &block->outgoing_entries);
	if (result != VDO_SUCCESS)
		return result;

	result = allocate_vio_components(allocator->depot->vdo,
					 VIO_TYPE_SLAB_SUMMARY,
					 VIO_PRIORITY_METADATA,
					 NULL,
					 1,
					 block->outgoing_entries,
					 &block->vio);
	if (result != VDO_SUCCESS)
		return result;

	block->allocator = allocator;
	block->entries = &allocator->summary_entries[VDO_SLAB_SUMMARY_ENTRIES_PER_BLOCK * index];
	block->index = index;
	return VDO_SUCCESS;
}

static int __must_check initialize_block_allocator(struct slab_depot *depot, zone_count_t zone)
{
	int result;
	block_count_t i;
	struct block_allocator *allocator = &depot->allocators[zone];
	struct vdo *vdo = depot->vdo;
	block_count_t max_free_blocks = depot->slab_config.data_blocks;
	unsigned int max_priority = (2 + ilog2(max_free_blocks));

	*allocator = (struct block_allocator) {
		.depot = depot,
		.zone_number = zone,
		.thread_id = vdo_get_physical_zone_thread(vdo->thread_config, zone),
		.nonce = vdo->states.vdo.nonce,
	};

	INIT_LIST_HEAD(&allocator->dirty_slab_journals);
	vdo_set_admin_state_code(&allocator->state, VDO_ADMIN_STATE_NORMAL_OPERATION);
	result = vdo_register_read_only_listener(vdo,
						 allocator,
						 notify_block_allocator_of_read_only_mode,
						 allocator->thread_id);
	if (result != VDO_SUCCESS)
		return result;

	vdo_initialize_completion(&allocator->completion, vdo, VDO_BLOCK_ALLOCATOR_COMPLETION);
	result = make_vio_pool(vdo,
			       BLOCK_ALLOCATOR_VIO_POOL_SIZE,
			       allocator->thread_id,
			       VIO_TYPE_SLAB_JOURNAL,
			       VIO_PRIORITY_METADATA,
			       allocator,
			       &allocator->vio_pool);
	if (result != VDO_SUCCESS)
		return result;

	result = initialize_slab_scrubber(allocator);
	if (result != VDO_SUCCESS)
		return result;

	result = make_priority_table(max_priority, &allocator->prioritized_slabs);
	if (result != VDO_SUCCESS)
		return result;

	result = UDS_ALLOCATE(VDO_SLAB_SUMMARY_BLOCKS_PER_ZONE,
			      struct slab_summary_block,
			      __func__,
			      &allocator->summary_blocks);
	if (result != VDO_SUCCESS)
		return result;

	vdo_set_admin_state_code(&allocator->summary_state, VDO_ADMIN_STATE_NORMAL_OPERATION);
	allocator->summary_entries = depot->summary_entries + (MAX_VDO_SLABS * zone);

	/* Initialize each summary block. */
	for (i = 0; i < VDO_SLAB_SUMMARY_BLOCKS_PER_ZONE; i++) {
		result = initialize_slab_summary_block(allocator, i);
		if (result != VDO_SUCCESS)
			return result;
	}

	/*
	 * Performing well atop thin provisioned storage requires either that VDO discards freed
	 * blocks, or that the block allocator try to use slabs that already have allocated blocks
	 * in preference to slabs that have never been opened. For reasons we have not been able to
	 * fully understand, some SSD machines have been have been very sensitive (50% reduction in
	 * test throughput) to very slight differences in the timing and locality of block
	 * allocation. Assigning a low priority to unopened slabs (max_priority/2, say) would be
	 * ideal for the story, but anything less than a very high threshold (max_priority - 1)
	 * hurts on these machines.
	 *
	 * This sets the free block threshold for preferring to open an unopened slab to the binary
	 * floor of 3/4ths the total number of data blocks in a slab, which will generally evaluate
	 * to about half the slab size.
	 */
#ifdef VDO_INTERNAL
	/*
	 * This also avoids degenerate behavior in unit tests where the number of data blocks is
	 * artificially constrained to a power of two.
	 */
#endif /* VDO_INTERNAL */
	allocator->unopened_slab_priority = (1 + ilog2((max_free_blocks * 3) / 4));

	return VDO_SUCCESS;
}

static int allocate_components(struct slab_depot *depot,
			       struct partition *summary_partition)
{
	int result;
	zone_count_t zone;
	slab_count_t slab_count;
	u8 hint;
	u32 i;
	const struct thread_config *thread_config = depot->vdo->thread_config;

	result = vdo_make_action_manager(depot->zone_count,
					 get_allocator_thread_id,
					 thread_config->journal_thread,
					 depot,
					 schedule_tail_block_commit,
					 depot->vdo,
					 &depot->action_manager);
	if (result != VDO_SUCCESS)
		return result;

	depot->origin = depot->first_block;

	/* block size must be a multiple of entry size */
	STATIC_ASSERT((VDO_BLOCK_SIZE % sizeof(struct slab_summary_entry)) == 0);

	vdo_set_slab_summary_origin(depot, summary_partition);
	depot->hint_shift = vdo_get_slab_summary_hint_shift(depot->slab_size_shift);
	result = UDS_ALLOCATE(MAXIMUM_VDO_SLAB_SUMMARY_ENTRIES,
			      struct slab_summary_entry,
			      __func__,
			      &depot->summary_entries);
	if (result != VDO_SUCCESS)
		return result;


	/* Initialize all the entries. */
	hint = compute_fullness_hint(depot, depot->slab_config.data_blocks);
	for (i = 0; i < MAXIMUM_VDO_SLAB_SUMMARY_ENTRIES; i++) {
		/*
		 * This default tail block offset must be reflected in
		 * slabJournal.c::read_slab_journal_tail().
		 */
		depot->summary_entries[i] = (struct slab_summary_entry) {
			.tail_block_offset = 0,
			.fullness_hint = hint,
			.load_ref_counts = false,
			.is_dirty = false,
		};
	}

	if (result != VDO_SUCCESS)
		return result;

	slab_count = vdo_compute_slab_count(depot->first_block,
					    depot->last_block,
					    depot->slab_size_shift);
	if (thread_config->physical_zone_count > slab_count)
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "%u physical zones exceeds slab count %u",
					      thread_config->physical_zone_count,
					      slab_count);

	/* Initialize the block allocators. */
	for (zone = 0; zone < depot->zone_count; zone++) {
		result = initialize_block_allocator(depot, zone);
		if (result != VDO_SUCCESS)
			return result;
	}

	/* Allocate slabs. */
	result = allocate_slabs(depot, slab_count);
	if (result != VDO_SUCCESS)
		return result;

	/* Use the new slabs. */
	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];

		register_slab_with_allocator(slab->allocator, slab);
		WRITE_ONCE(depot->slab_count, depot->slab_count + 1);
	}

	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;

	return VDO_SUCCESS;
}

/**
 * vdo_decode_slab_depot() - Make a slab depot and configure it with the state read from the super
 *                           block.
 * @state: The slab depot state from the super block.
 * @vdo: The VDO which will own the depot.
 * @summary_partition: The partition which holds the slab summary.
 * @depot_ptr: A pointer to hold the depot.
 *
 * Return: A success or error code.
 */
int vdo_decode_slab_depot(struct slab_depot_state_2_0 state,
			  struct vdo *vdo,
			  struct partition *summary_partition,
			  struct slab_depot **depot_ptr)
{
	unsigned int slab_size_shift;
	struct slab_depot *depot;
	int result;

	/*
	 * Calculate the bit shift for efficiently mapping block numbers to slabs. Using a shift
	 * requires that the slab size be a power of two.
	 */
	block_count_t slab_size = state.slab_config.slab_blocks;

	if (!is_power_of_2(slab_size))
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "slab size must be a power of two");
	slab_size_shift = ilog2(slab_size);

	result = UDS_ALLOCATE_EXTENDED(struct slab_depot,
				       vdo->thread_config->physical_zone_count,
				       struct block_allocator,
				       __func__,
				       &depot);
	if (result != VDO_SUCCESS)
		return result;

	depot->vdo = vdo;
	depot->old_zone_count = state.zone_count;
	depot->zone_count = vdo->thread_config->physical_zone_count;
	depot->slab_config = state.slab_config;
	depot->first_block = state.first_block;
	depot->last_block = state.last_block;
	depot->slab_size_shift = slab_size_shift;

	result = allocate_components(depot, summary_partition);
	if (result != VDO_SUCCESS) {
		vdo_free_slab_depot(depot);
		return result;
	}

	*depot_ptr = depot;
	return VDO_SUCCESS;
}

static void uninitialize_allocator_summary(struct block_allocator *allocator)
{
	block_count_t i;

	if (allocator->summary_blocks == NULL)
		return;

	for (i = 0; i < VDO_SLAB_SUMMARY_BLOCKS_PER_ZONE; i++) {
		free_vio_components(&allocator->summary_blocks[i].vio);
		UDS_FREE(UDS_FORGET(allocator->summary_blocks[i].outgoing_entries));
	}

	UDS_FREE(UDS_FORGET(allocator->summary_blocks));
}

/**
 * vdo_free_slab_depot() - Destroy a slab depot.
 * @depot: The depot to destroy.
 */
void vdo_free_slab_depot(struct slab_depot *depot)
{
	zone_count_t zone = 0;

	if (depot == NULL)
		return;

	vdo_abandon_new_slabs(depot);

	for (zone = 0; zone < depot->zone_count; zone++) {
		struct block_allocator *allocator = &depot->allocators[zone];

		if (allocator->eraser != NULL)
			dm_kcopyd_client_destroy(UDS_FORGET(allocator->eraser));

		uninitialize_allocator_summary(allocator);
		uninitialize_scrubber_vio(&allocator->scrubber);
		free_vio_pool(UDS_FORGET(allocator->vio_pool));
		free_priority_table(UDS_FORGET(allocator->prioritized_slabs));
	}

	if (depot->slabs != NULL) {
		slab_count_t i;

		for (i = 0; i < depot->slab_count; i++)
			vdo_free_slab(UDS_FORGET(depot->slabs[i]));
	}

	UDS_FREE(UDS_FORGET(depot->slabs));
	UDS_FREE(UDS_FORGET(depot->action_manager));
	UDS_FREE(UDS_FORGET(depot->summary_entries));
	UDS_FREE(depot);
}

/**
 * vdo_record_slab_depot() - Record the state of a slab depot for encoding into the super block.
 * @depot: The depot to encode.
 *
 * Return: The depot state.
 */
struct slab_depot_state_2_0 vdo_record_slab_depot(const struct slab_depot *depot)
{
	/*
	 * If this depot is currently using 0 zones, it must have been synchronously loaded by a
	 * tool and is now being saved. We did not load and combine the slab summary, so we still
	 * need to do that next time we load with the old zone count rather than 0.
	 */
	struct slab_depot_state_2_0 state;
	zone_count_t zones_to_record = depot->zone_count;

	if (depot->zone_count == 0)
		zones_to_record = depot->old_zone_count;

	state = (struct slab_depot_state_2_0) {
		.slab_config = depot->slab_config,
		.first_block = depot->first_block,
		.last_block = depot->last_block,
		.zone_count = zones_to_record,
	};

	return state;
}

/**
 * vdo_allocate_slab_ref_counts() - Allocate the ref_counts for all slabs in the depot.
 * @depot: The depot whose ref_counts need allocation.
 *
 * Context: This method may be called only before entering normal operation from the load thread.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_allocate_slab_ref_counts(struct slab_depot *depot)
{
	struct slab_iterator iterator =
		get_depot_slab_iterator(depot, depot->slab_count - 1, 0, 1);

	while (iterator.next != NULL) {
		int result = vdo_allocate_ref_counts_for_slab(next_slab(&iterator));

		if (result != VDO_SUCCESS)
			return result;
	}

	return VDO_SUCCESS;
}

/**
 * get_slab_number() - Get the number of the slab that contains a specified block.
 * @depot: The slab depot.
 * @pbn: The physical block number.
 * @slab_number_ptr: A pointer to hold the slab number.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check get_slab_number(const struct slab_depot *depot,
					physical_block_number_t pbn,
					slab_count_t *slab_number_ptr)
{
	slab_count_t slab_number;

	if (pbn < depot->first_block)
		return VDO_OUT_OF_RANGE;

	slab_number = (pbn - depot->first_block) >> depot->slab_size_shift;
	if (slab_number >= depot->slab_count)
		return VDO_OUT_OF_RANGE;

	*slab_number_ptr = slab_number;
	return VDO_SUCCESS;
}

/**
 * vdo_get_slab() - Get the slab object for the slab that contains a specified block.
 * @depot: The slab depot.
 * @pbn: The physical block number.
 *
 * Will put the VDO in read-only mode if the PBN is not a valid data block nor the zero block.
 *
 * Return: The slab containing the block, or NULL if the block number is the zero block or
 * otherwise out of range.
 */
struct vdo_slab *vdo_get_slab(const struct slab_depot *depot, physical_block_number_t pbn)
{
	slab_count_t slab_number;
	int result;

	if (pbn == VDO_ZERO_BLOCK)
		return NULL;

	result = get_slab_number(depot, pbn, &slab_number);
	if (result != VDO_SUCCESS) {
		vdo_enter_read_only_mode(depot->vdo, result);
		return NULL;
	}

	return depot->slabs[slab_number];
}

/**
 * vdo_get_increment_limit() - Determine how many new references a block can acquire.
 * @depot: The slab depot.
 * @pbn: The physical block number that is being queried.
 *
 * Context: This method must be called from the physical zone thread of the PBN.
 *
 * Return: The number of available references.
 */
u8 vdo_get_increment_limit(struct slab_depot *depot, physical_block_number_t pbn)
{
	struct vdo_slab *slab = vdo_get_slab(depot, pbn);

	if ((slab == NULL) || (slab->status != VDO_SLAB_REBUILT))
		return 0;

	return vdo_get_available_references(slab->reference_counts, pbn);
}

/**
 * vdo_is_physical_data_block() - Determine whether the given PBN refers to a data block.
 * @depot: The depot.
 * @pbn: The physical block number to ask about.
 *
 * Return: True if the PBN corresponds to a data block.
 */
bool vdo_is_physical_data_block(const struct slab_depot *depot, physical_block_number_t pbn)
{
	slab_count_t slab_number;
	slab_block_number sbn;

	return ((pbn == VDO_ZERO_BLOCK) ||
		((get_slab_number(depot, pbn, &slab_number) == VDO_SUCCESS) &&
		 (vdo_slab_block_number_from_pbn(depot->slabs[slab_number], pbn, &sbn) ==
		  VDO_SUCCESS)));
}

/**
 * vdo_get_slab_depot_allocated_blocks() - Get the total number of data blocks allocated across all
 * the slabs in the depot.
 * @depot: The slab depot.
 *
 * This is the total number of blocks with a non-zero reference count.
 *
 * Context: This may be called from any thread.
 *
 * Return: The total number of blocks with a non-zero reference count.
 */
block_count_t vdo_get_slab_depot_allocated_blocks(const struct slab_depot *depot)
{
	block_count_t total = 0;
	zone_count_t zone;

	for (zone = 0; zone < depot->zone_count; zone++)
		/* The allocators are responsible for thread safety. */
		total += READ_ONCE(depot->allocators[zone].allocated_blocks);
	return total;
}

/**
 * vdo_get_slab_depot_data_blocks() - Get the total number of data blocks in all the slabs in the
 *                                    depot.
 * @depot: The slab depot.
 *
 * Context: This may be called from any thread.
 *
 * Return: The total number of data blocks in all slabs.
 */
block_count_t vdo_get_slab_depot_data_blocks(const struct slab_depot *depot)
{
	return (READ_ONCE(depot->slab_count) * depot->slab_config.data_blocks);
}

/**
 * finish_combining_zones() - Clean up after saving out the combined slab summary.
 * @completion: The vio which was used to write the summary data.
 */
static void finish_combining_zones(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vdo_completion *parent = completion->parent;

	free_vio(as_vio(UDS_FORGET(completion)));
	vdo_fail_completion(parent, result);
}

static void handle_combining_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	finish_combining_zones(completion);
}

static void write_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vdo *vdo = vio->completion.vdo;

	continue_vio_after_io(vio, finish_combining_zones, vdo->thread_config->admin_thread);
}

/**
 * combine_summaries() - Treating the current entries buffer as the on-disk value of all zones,
 *                       update every zone to the correct values for every slab.
 * @depot: The depot whose summary entries should be combined.
 */
static void combine_summaries(struct slab_depot *depot)
{
	/*
	 * Combine all the old summary data into the portion of the buffer corresponding to the
	 * first zone.
	 */
	zone_count_t zone = 0;
	struct slab_summary_entry *entries = depot->summary_entries;

	if (depot->old_zone_count > 1) {
		slab_count_t entry_number;

		for (entry_number = 0; entry_number < MAX_VDO_SLABS; entry_number++) {
			if (zone != 0)
				memcpy(entries + entry_number,
				       entries + (zone * MAX_VDO_SLABS) + entry_number,
				       sizeof(struct slab_summary_entry));
			zone++;
			if (zone == depot->old_zone_count)
				zone = 0;
		}
	}

	/* Copy the combined data to each zones's region of the buffer. */
	for (zone = 1; zone < MAX_VDO_PHYSICAL_ZONES; zone++)
		memcpy(entries + (zone * MAX_VDO_SLABS),
		       entries,
		       MAX_VDO_SLABS * sizeof(struct slab_summary_entry));
}

/**
 * finish_loading_summary() - Finish loading slab summary data.
 * @completion: The vio which was used to read the summary data.
 *
 * Combines the slab summary data from all the previously written zones and copies the combined
 * summary to each partition's data region. Then writes the combined summary back out to disk. This
 * callback is registered in load_summary_endio().
 */
static void finish_loading_summary(struct vdo_completion *completion)
{
	struct slab_depot *depot = completion->vdo->depot;

	/* Combine the summary from each zone so each zone is correct for all slabs. */
	combine_summaries(depot);

	/* Write the combined summary back out. */
	submit_metadata_vio(as_vio(completion),
			    depot->summary_origin,
			    write_summary_endio,
			    handle_combining_error,
			    REQ_OP_WRITE);
}

static void load_summary_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vdo *vdo = vio->completion.vdo;

	continue_vio_after_io(vio, finish_loading_summary, vdo->thread_config->admin_thread);
}

/**
 * load_slab_summary() - The preamble of a load operation.
 *
 * Implements vdo_action_preamble.
 */
EXTERNAL_STATIC void load_slab_summary(void *context, struct vdo_completion *parent)
{
	int result;
	struct vio *vio;
	struct slab_depot *depot = context;
	const struct admin_state_code *operation =
		vdo_get_current_manager_operation(depot->action_manager);

	result = create_multi_block_metadata_vio(depot->vdo,
						 VIO_TYPE_SLAB_SUMMARY,
						 VIO_PRIORITY_METADATA,
						 parent,
						 VDO_SLAB_SUMMARY_BLOCKS,
						 (char *) depot->summary_entries,
						 &vio);
	if (result != VDO_SUCCESS)
		vdo_fail_completion(parent, result);

	if ((operation == VDO_ADMIN_STATE_FORMATTING) ||
	    (operation == VDO_ADMIN_STATE_LOADING_FOR_REBUILD)) {
		finish_loading_summary(&vio->completion);
		return;
	}

	submit_metadata_vio(vio,
			    depot->summary_origin,
			    load_summary_endio,
			    handle_combining_error,
			    REQ_OP_READ);
}

/* Implements vdo_zone_action. */
static void load_allocator(void *context, zone_count_t zone_number, struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	vdo_start_loading(&depot->allocators[zone_number].state,
			  vdo_get_current_manager_operation(depot->action_manager),
			  parent,
			  initiate_load);
}

/**
 * vdo_load_slab_depot() - Asynchronously load any slab depot state that isn't included in the
 *                         super_block component.
 * @depot: The depot to load.
 * @operation: The type of load to perform.
 * @parent: The completion to notify when the load is complete.
 * @context: Additional context for the load operation; may be NULL.
 *
 * This method may be called only before entering normal operation from the load thread.
 */
void vdo_load_slab_depot(struct slab_depot *depot,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent,
			 void *context)
{
	if (vdo_assert_load_operation(operation, parent))
		vdo_schedule_operation_with_context(depot->action_manager,
						    operation,
						    load_slab_summary,
						    load_allocator,
						    NULL,
						    context,
						    parent);
}

/* Implements vdo_zone_action. */
static void prepare_to_allocate(void *context,
				zone_count_t zone_number,
				struct vdo_completion *parent)
{
	struct slab_depot *depot = context;
	struct block_allocator *allocator = &depot->allocators[zone_number];
	int result;

	result = vdo_prepare_slabs_for_allocation(allocator);
	if (result != VDO_SUCCESS) {
		vdo_fail_completion(parent, result);
		return;
	}

	scrub_slabs(allocator, parent);
}

/**
 * vdo_prepare_slab_depot_to_allocate() - Prepare the slab depot to come online and start
 *                                        allocating blocks.
 * @depot: The depot to prepare.
 * @load_type: The load type.
 * @parent: The completion to notify when the operation is complete.
 *
 * This method may be called only before entering normal operation from the load thread. It must be
 * called before allocation may proceed.
 */
void vdo_prepare_slab_depot_to_allocate(struct slab_depot *depot,
					enum slab_depot_load_type load_type,
					struct vdo_completion *parent)
{
	depot->load_type = load_type;
	atomic_set(&depot->zones_to_scrub, depot->zone_count);
	vdo_schedule_action(depot->action_manager, NULL, prepare_to_allocate, NULL, parent);
}

/**
 * vdo_update_slab_depot_size() - Update the slab depot to reflect its new size in memory.
 * @depot: The depot to update.
 *
 * This size is saved to disk as part of the super block.
 */
void vdo_update_slab_depot_size(struct slab_depot *depot)
{
	depot->last_block = depot->new_last_block;
}

/**
 * vdo_prepare_to_grow_slab_depot() - Allocate new memory needed for a resize of a slab depot to
 *                                    the given size.
 * @depot: The depot to prepare to resize.
 * @new_size: The number of blocks in the new depot.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size)
{
	struct slab_depot_state_2_0 new_state;
	int result;
	slab_count_t new_slab_count;

	if ((new_size >> depot->slab_size_shift) <= depot->slab_count)
		return VDO_INCREMENT_TOO_SMALL;

	/* Generate the depot configuration for the new block count. */
	result = vdo_configure_slab_depot(new_size,
					  depot->first_block,
					  depot->slab_config,
					  depot->zone_count,
					  &new_state);
	if (result != VDO_SUCCESS)
		return result;

	new_slab_count = vdo_compute_slab_count(depot->first_block,
						new_state.last_block,
						depot->slab_size_shift);
	if (new_slab_count <= depot->slab_count)
		return uds_log_error_strerror(VDO_INCREMENT_TOO_SMALL, "Depot can only grow");
	if (new_slab_count == depot->new_slab_count)
		/* Check it out, we've already got all the new slabs allocated! */
		return VDO_SUCCESS;

	vdo_abandon_new_slabs(depot);
	result = allocate_slabs(depot, new_slab_count);
	if (result != VDO_SUCCESS) {
		vdo_abandon_new_slabs(depot);
		return result;
	}

	depot->new_size = new_size;
	depot->old_last_block = depot->last_block;
	depot->new_last_block = new_state.last_block;

	return VDO_SUCCESS;
}

/**
 * finish_registration() - Finish registering new slabs now that all of the allocators have
 *                         received their new slabs.
 *
 * Implements vdo_action_conclusion.
 */
static int finish_registration(void *context)
{
	struct slab_depot *depot = context;

	WRITE_ONCE(depot->slab_count, depot->new_slab_count);
	UDS_FREE(depot->slabs);
	depot->slabs = depot->new_slabs;
	depot->new_slabs = NULL;
	depot->new_slab_count = 0;
	return VDO_SUCCESS;
}

/* Implements vdo_zone_action. */
static void register_new_slabs(void *context,
			       zone_count_t zone_number,
			       struct vdo_completion *parent)
{
	struct slab_depot *depot = context;
	struct block_allocator *allocator = &depot->allocators[zone_number];
	slab_count_t i;

	for (i = depot->slab_count; i < depot->new_slab_count; i++) {
		struct vdo_slab *slab = depot->new_slabs[i];

		if (slab->allocator == allocator)
			register_slab_with_allocator(allocator, slab);
	}

	vdo_finish_completion(parent);
}

/**
 * vdo_use_new_slabs() - Use the new slabs allocated for resize.
 * @depot: The depot.
 * @parent: The object to notify when complete.
 */
void vdo_use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent)
{
	ASSERT_LOG_ONLY(depot->new_slabs != NULL, "Must have new slabs to use");
	vdo_schedule_operation(depot->action_manager,
			       VDO_ADMIN_STATE_SUSPENDED_OPERATION,
			       NULL,
			       register_new_slabs,
			       finish_registration,
			       parent);
}

/**
 * stop_scrubbing() - Tell the scrubber to stop scrubbing after it finishes the slab it is
 *                    currently working on.
 * @scrubber: The scrubber to stop.
 * @parent: The completion to notify when scrubbing has stopped.
 */
EXTERNAL_STATIC void stop_scrubbing(struct block_allocator *allocator)
{
	struct slab_scrubber *scrubber = &allocator->scrubber;

	if (vdo_is_state_quiescent(&scrubber->admin_state))
		vdo_finish_completion(&allocator->completion);
	else
		vdo_start_draining(&scrubber->admin_state,
				   VDO_ADMIN_STATE_SUSPENDING,
				   &allocator->completion,
				   NULL);
}

/**
 * Implements vdo_admin_initiator.
 */
EXTERNAL_STATIC void initiate_summary_drain(struct admin_state *state)
{
	check_summary_drain_complete(container_of(state, struct block_allocator, summary_state));
}

static void do_drain_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);

	vdo_prepare_completion_for_requeue(&allocator->completion,
					   do_drain_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (++allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		stop_scrubbing(allocator);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_drain_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		vdo_start_draining(&allocator->summary_state,
				   vdo_get_admin_state_code(&allocator->state),
				   completion,
				   initiate_summary_drain);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_FINISHED:
		ASSERT_LOG_ONLY(!is_vio_pool_busy(allocator->vio_pool), "vio pool not busy");
		vdo_finish_draining_with_result(&allocator->state, completion->result);
		return;

	default:
		vdo_finish_draining_with_result(&allocator->state, UDS_BAD_STATE);
	}
}

/* Implements vdo_admin_initiator. */
static void initiate_drain(struct admin_state *state)
{
	struct block_allocator *allocator = container_of(state, struct block_allocator, state);

	allocator->drain_step = VDO_DRAIN_ALLOCATOR_START;
	do_drain_step(&allocator->completion);
}

/*
 * Drain all allocator I/O. Depending upon the type of drain, some or all dirty metadata may be
 * written to disk. The type of drain will be determined from the state of the allocator's depot.
 *
 * Implements vdo_zone_action.
 */
static void drain_allocator(void *context, zone_count_t zone_number, struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	vdo_start_draining(&depot->allocators[zone_number].state,
			   vdo_get_current_manager_operation(depot->action_manager),
			   parent,
			   initiate_drain);
}

/**
 * vdo_drain_slab_depot() - Drain all slab depot I/O.
 * @depot: The depot to drain.
 * @operation: The drain operation (flush, rebuild, suspend, or save).
 * @parent: The completion to finish when the drain is complete.
 *
 * If saving, or flushing, all dirty depot metadata will be written out. If saving or suspending,
 * the depot will be left in a suspended state.
 */
void vdo_drain_slab_depot(struct slab_depot *depot,
			  const struct admin_state_code *operation,
			  struct vdo_completion *parent)
{
	vdo_schedule_operation(depot->action_manager,
			       operation,
			       NULL,
			       drain_allocator,
			       NULL,
			       parent);
}

/**
 * resume_scrubbing() - Tell the scrubber to resume scrubbing if it has been stopped.
 * @alocator: The allocator being resumed.
 */
static void resume_scrubbing(struct block_allocator *allocator)
{
	int result;
	struct slab_scrubber *scrubber = &allocator->scrubber;

	if (!has_slabs_to_scrub(scrubber)) {
		vdo_finish_completion(&allocator->completion);
		return;
	}

	result = vdo_resume_if_quiescent(&scrubber->admin_state);
	if (result != VDO_SUCCESS) {
		vdo_fail_completion(&allocator->completion, result);
		return;
	}

	scrub_next_slab(scrubber);
	vdo_finish_completion(&allocator->completion);
}

static void do_resume_step(struct vdo_completion *completion)
{
	struct block_allocator *allocator = vdo_as_block_allocator(completion);

	vdo_prepare_completion_for_requeue(&allocator->completion,
					   do_resume_step,
					   handle_operation_error,
					   allocator->thread_id,
					   NULL);
	switch (--allocator->drain_step) {
	case VDO_DRAIN_ALLOCATOR_STEP_SUMMARY:
		vdo_fail_completion(completion,
				    vdo_resume_if_quiescent(&allocator->summary_state));
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SLABS:
		apply_to_slabs(allocator, do_resume_step);
		return;

	case VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER:
		resume_scrubbing(allocator);
		return;

	case VDO_DRAIN_ALLOCATOR_START:
		vdo_finish_resuming_with_result(&allocator->state, completion->result);
		return;

	default:
		vdo_finish_resuming_with_result(&allocator->state, UDS_BAD_STATE);
	}
}

/* Implements vdo_admin_initiator. */
static void initiate_resume(struct admin_state *state)
{
	struct block_allocator *allocator = container_of(state, struct block_allocator, state);

	allocator->drain_step = VDO_DRAIN_ALLOCATOR_STEP_FINISHED;
	do_resume_step(&allocator->completion);
}

/* Implements vdo_zone_action. */
static void resume_allocator(void *context,
			     zone_count_t zone_number,
			     struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	vdo_start_resuming(&depot->allocators[zone_number].state,
			   vdo_get_current_manager_operation(depot->action_manager),
			   parent,
			   initiate_resume);
}

/**
 * vdo_resume_slab_depot() - Resume a suspended slab depot.
 * @depot: The depot to resume.
 * @parent: The completion to finish when the depot has resumed.
 */
void vdo_resume_slab_depot(struct slab_depot *depot, struct vdo_completion *parent)
{
	if (vdo_is_read_only(depot->vdo)) {
		vdo_continue_completion(parent, VDO_READ_ONLY);
		return;
	}

	vdo_schedule_operation(depot->action_manager,
			       VDO_ADMIN_STATE_RESUMING,
			       NULL,
			       resume_allocator,
			       NULL,
			       parent);
}

/**
 * vdo_commit_oldest_slab_journal_tail_blocks() - Commit all dirty tail blocks which are locking a
 *                                                given recovery journal block.
 * @depot: The depot.
 * @recovery_block_number: The sequence number of the recovery journal block whose locks should be
 *                         released.
 *
 * Context: This method must be called from the journal zone thread.
 */
void vdo_commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
						sequence_number_t recovery_block_number)
{
	if (depot == NULL)
		return;

	depot->new_release_request = recovery_block_number;
	vdo_schedule_default_action(depot->action_manager);
}

/* Implements vdo_zone_action. */
static void scrub_all_unrecovered_slabs(void *context,
					zone_count_t zone_number,
					struct vdo_completion *parent)
{
	struct slab_depot *depot = context;

	scrub_slabs(&depot->allocators[zone_number], NULL);
	vdo_launch_completion(parent);
}

/**
 * vdo_scrub_all_unrecovered_slabs() - Scrub all unrecovered slabs.
 * @depot: The depot to scrub.
 * @parent: The object to notify when scrubbing has been launched for all zones.
 */
void vdo_scrub_all_unrecovered_slabs(struct slab_depot *depot, struct vdo_completion *parent)
{
	vdo_schedule_action(depot->action_manager,
			    NULL,
			    scrub_all_unrecovered_slabs,
			    NULL,
			    parent);
}

/**
 * get_block_allocator_statistics() - Get the total of the statistics from all the block
 *                                          allocators in the depot.
 * @depot: The slab depot.
 *
 * Return: The statistics from all block allocators in the depot.
 */
static struct block_allocator_statistics __must_check
get_block_allocator_statistics(const struct slab_depot *depot)
{
	struct block_allocator_statistics totals;
	zone_count_t zone;

	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < depot->zone_count; zone++) {
		const struct block_allocator *allocator = &depot->allocators[zone];
		const struct block_allocator_statistics *stats = &allocator->statistics;

		totals.slab_count += allocator->slab_count;
		totals.slabs_opened += READ_ONCE(stats->slabs_opened);
		totals.slabs_reopened += READ_ONCE(stats->slabs_reopened);
	}

	return totals;
}

/**
 * get_ref_counts_statistics() - Get the cumulative ref_counts statistics for the depot.
 * @depot: The slab depot.
 *
 * Return: The cumulative statistics for all ref_counts in the depot.
 */
static struct ref_counts_statistics __must_check
get_ref_counts_statistics(const struct slab_depot *depot)
{
	struct ref_counts_statistics totals;
	zone_count_t zone;

	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < depot->zone_count; zone++) {
		totals.blocks_written +=
			READ_ONCE(depot->allocators[zone].ref_counts_statistics.blocks_written);
	}

	return totals;
}

/**
 * get_depot_slab_journal_statistics() - Get the aggregated slab journal statistics for the depot.
 * @depot: The slab depot.
 *
 * Return: The aggregated statistics for all slab journals in the depot.
 */
static struct slab_journal_statistics __must_check
get_slab_journal_statistics(const struct slab_depot *depot)
{
	struct slab_journal_statistics totals;
	zone_count_t zone;

	memset(&totals, 0, sizeof(totals));

	for (zone = 0; zone < depot->zone_count; zone++) {
		const struct slab_journal_statistics *stats =
			&depot->allocators[zone].slab_journal_statistics;

		totals.disk_full_count += READ_ONCE(stats->disk_full_count);
		totals.flush_count += READ_ONCE(stats->flush_count);
		totals.blocked_count += READ_ONCE(stats->blocked_count);
		totals.blocks_written += READ_ONCE(stats->blocks_written);
		totals.tail_busy_count += READ_ONCE(stats->tail_busy_count);
	}

	return totals;
}

/**
 * vdo_get_slab_depot_statistics() - Get all the vdo_statistics fields that are properties of the
 *                                   slab depot.
 * @depot: The slab depot.
 * @stats: The vdo statistics structure to partially fill.
 */
void vdo_get_slab_depot_statistics(const struct slab_depot *depot, struct vdo_statistics *stats)
{
	slab_count_t slab_count = READ_ONCE(depot->slab_count);
	slab_count_t unrecovered = 0;
	zone_count_t zone;

	for (zone = 0; zone < depot->zone_count; zone++) {
		/* The allocators are responsible for thread safety. */
		unrecovered += READ_ONCE(depot->allocators[zone].scrubber.slab_count);
	}

	stats->recovery_percentage = (slab_count - unrecovered) * 100 / slab_count;
	stats->allocator = get_block_allocator_statistics(depot);
	stats->ref_counts = get_ref_counts_statistics(depot);
	stats->slab_journal = get_slab_journal_statistics(depot);
	stats->slab_summary = (struct slab_summary_statistics) {
		.blocks_written = atomic64_read(&depot->summary_statistics.blocks_written),
	};
}

/**
 * vdo_dump_slab_depot() - Dump the slab depot, in a thread-unsafe fashion.
 * @depot: The slab depot.
 */
void vdo_dump_slab_depot(const struct slab_depot *depot)
{
	uds_log_info("vdo slab depot");
	uds_log_info("  zone_count=%u old_zone_count=%u slabCount=%u active_release_request=%llu new_release_request=%llu",
		     (unsigned int) depot->zone_count,
		     (unsigned int) depot->old_zone_count,
		     READ_ONCE(depot->slab_count),
		     (unsigned long long) depot->active_release_request,
		     (unsigned long long) depot->new_release_request);
}
