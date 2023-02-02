// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "block-map.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "action-manager.h"
#include "admin-state.h"
#include "constants.h"
#include "data-vio.h"
#include "dirty-lists.h"
#include "forest.h"
#include "io-submitter.h"
#include "physical-zone.h"
#include "recovery-journal.h"
#include "reference-operation.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"
#include "vdo-component-states.h"
#include "vdo-page-cache.h"
#include "vio.h"
#include "wait-queue.h"

struct page_descriptor {
	root_count_t root_index;
	height_t height;
	page_number_t page_index;
	slot_number_t slot;
} __packed;

union page_key {
	struct page_descriptor descriptor;
	u64 key;
};

struct write_if_not_dirtied_context {
	struct block_map_zone *zone;
	u8 generation;
};

/* Used to indicate that the page holding the location of a tree root has been "loaded". */
const physical_block_number_t VDO_INVALID_PBN = 0xFFFFFFFFFFFFFFFF;

static void write_dirty_pages_callback(struct list_head *expired, void *context);

static inline struct block_map_zone * __must_check
get_block_map_zone(struct data_vio *data_vio)
{
	return data_vio->logical.zone->block_map_zone;
}

/* Get the page referred to by the lock's tree slot at its current height. */
static inline struct tree_page *
get_tree_page(const struct block_map_zone *zone, const struct tree_lock *lock)
{
	return vdo_get_tree_page_by_index(zone->block_map->forest,
					  lock->root_index,
					  lock->height,
					  lock->tree_slots[lock->height].page_index);
}

/**
 * vdo_copy_valid_page() - Validate and copy a buffer to a page.
 * @pbn: The expected PBN.
 */
bool vdo_copy_valid_page(char *buffer,
			 nonce_t nonce,
			 physical_block_number_t pbn,
			 struct block_map_page *page)
{
	struct block_map_page *loaded = (struct block_map_page *) buffer;
	enum block_map_page_validity validity = vdo_validate_block_map_page(loaded, nonce, pbn);

	if (validity == VDO_BLOCK_MAP_PAGE_VALID) {
		memcpy(page, loaded, VDO_BLOCK_SIZE);
		return true;
	}

	if (validity == VDO_BLOCK_MAP_PAGE_BAD)
		uds_log_error_strerror(VDO_BAD_PAGE,
				       "Expected page %llu but got page %llu instead",
				       (unsigned long long) pbn,
				       (unsigned long long) vdo_get_block_map_page_pbn(loaded));

	return false;
}

void vdo_block_map_check_for_drain_complete(struct block_map_zone *zone)
{
	if (vdo_is_state_draining(&zone->state) &&
	    (zone->active_lookups == 0) &&
	    !has_waiters(&zone->flush_waiters) &&
	    !is_vio_pool_busy(zone->vio_pool) &&
	    !vdo_is_page_cache_active(zone->page_cache))
		vdo_finish_draining_with_result(&zone->state,
						(vdo_is_read_only(zone->read_only_notifier) ?
						 VDO_READ_ONLY : VDO_SUCCESS));
}

static void enter_zone_read_only_mode(struct block_map_zone *zone, int result)
{
	vdo_enter_read_only_mode(zone->read_only_notifier, result);

	/*
	 * We are in read-only mode, so we won't ever write any page out. Just take all waiters off
	 * the queue so the zone can drain.
	 */
	while (has_waiters(&zone->flush_waiters))
		dequeue_next_waiter(&zone->flush_waiters);

	vdo_block_map_check_for_drain_complete(zone);
}

/**
 * in_cyclic_range() - Check whether the given value is between the lower and upper bounds, within
 *                     a cyclic range of values from 0 to (modulus - 1).
 * @lower: The lowest value to accept.
 * @value: The value to check.
 * @upper: The highest value to accept.
 * @modulus: The size of the cyclic space, no more than 2^15.
 *
 * The value and both bounds must be smaller than the modulus.
 *
 * Return: true if the value is in range.
 */
EXTERNAL_STATIC bool in_cyclic_range(u16 lower, u16 value, u16 upper, u16 modulus)
{
	if (value < lower)
		value += modulus;
	if (upper < lower)
		upper += modulus;
	return (value <= upper);
}

/**
 * is_not_older() - Check whether a generation is strictly older than some other generation in the
 *                  context of a zone's current generation range.
 * @zone: The zone in which to do the comparison.
 * @a: The generation in question.
 * @b: The generation to compare to.
 *
 * Return: true if generation @a is not strictly older than generation @b in the context of @zone
 */
static bool __must_check is_not_older(struct block_map_zone *zone, u8 a, u8 b)
{
	int result;

	result = ASSERT((in_cyclic_range(zone->oldest_generation, a, zone->generation, 1 << 8) &&
			 in_cyclic_range(zone->oldest_generation, b, zone->generation, 1 << 8)),
			"generation(s) %u, %u are out of range [%u, %u]",
			a, b, zone->oldest_generation, zone->generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return true;
	}

	return in_cyclic_range(b, a, zone->generation, 1 << 8);
}

static void release_generation(struct block_map_zone *zone, u8 generation)
{
	int result;

	result = ASSERT((zone->dirty_page_counts[generation] > 0),
			"dirty page count underflow for generation %u",
			generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	zone->dirty_page_counts[generation]--;
	while ((zone->dirty_page_counts[zone->oldest_generation] == 0) &&
	       (zone->oldest_generation != zone->generation))
		zone->oldest_generation++;
}

static void
set_generation(struct block_map_zone *zone, struct tree_page *page, u8 new_generation)
{
	u32 new_count;
	int result;
	bool decrement_old = is_waiting(&page->waiter);
	u8 old_generation = page->generation;

	if (decrement_old && (old_generation == new_generation))
		return;

	page->generation = new_generation;
	new_count = ++zone->dirty_page_counts[new_generation];
	result = ASSERT((new_count != 0),
			"dirty page count overflow for generation %u",
			new_generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	if (decrement_old)
		release_generation(zone, old_generation);
}

static void write_page(struct tree_page *tree_page, struct pooled_vio *vio);

/* Implements waiter_callback */
static void write_page_callback(struct waiter *waiter, void *context)
{
	write_page(container_of(waiter, struct tree_page, waiter), (struct pooled_vio *) context);
}

static void acquire_vio(struct waiter *waiter, struct block_map_zone *zone)
{
	waiter->callback = write_page_callback;
	acquire_vio_from_pool(zone->vio_pool, waiter);
}

/* Return: true if all possible generations were not already active */
static bool attempt_increment(struct block_map_zone *zone)
{
	u8 generation = zone->generation + 1;

	if (zone->oldest_generation == generation)
		return false;

	zone->generation = generation;
	return true;
}

/* Launches a flush if one is not already in progress. */
static void enqueue_page(struct tree_page *page, struct block_map_zone *zone)
{
	if ((zone->flusher == NULL) && attempt_increment(zone)) {
		zone->flusher = page;
		acquire_vio(&page->waiter, zone);
		return;
	}

	enqueue_waiter(&zone->flush_waiters, &page->waiter);
}

static void write_page_if_not_dirtied(struct waiter *waiter, void *context)
{
	struct tree_page *page = container_of(waiter, struct tree_page, waiter);
	struct write_if_not_dirtied_context *write_context = context;

	if (page->generation == write_context->generation) {
		acquire_vio(waiter, write_context->zone);
		return;
	}

	enqueue_page(page, write_context->zone);
}

static void return_to_pool(struct block_map_zone *zone, struct pooled_vio *vio)
{
	return_vio_to_pool(zone->vio_pool, vio);
	vdo_block_map_check_for_drain_complete(zone);
}

/* This callback is registered in write_initialized_page(). */
static void finish_page_write(struct vdo_completion *completion)
{
	bool dirty;
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = container_of(vio, struct pooled_vio, vio);
	struct tree_page *page = completion->parent;
	struct block_map_zone *zone = pooled->context;

	vdo_release_recovery_journal_block_reference(zone->block_map->journal,
						     page->writing_recovery_lock,
						     VDO_ZONE_TYPE_LOGICAL,
						     zone->zone_number);

	dirty = (page->writing_generation != page->generation);
	release_generation(zone, page->writing_generation);
	page->writing = false;

	if (zone->flusher == page) {
		struct write_if_not_dirtied_context context = {
			.zone = zone,
			.generation = page->writing_generation,
		};

		notify_all_waiters(&zone->flush_waiters, write_page_if_not_dirtied, &context);
		if (dirty && attempt_increment(zone)) {
			write_page(page, pooled);
			return;
		}

		zone->flusher = NULL;
	}

	if (dirty) {
		enqueue_page(page, zone);
	} else if ((zone->flusher == NULL) &&
		   has_waiters(&zone->flush_waiters) &&
		   attempt_increment(zone)) {
		zone->flusher = container_of(dequeue_next_waiter(&zone->flush_waiters),
					     struct tree_page,
					     waiter);
		write_page(zone->flusher, pooled);
		return;
	}

	return_to_pool(zone, pooled);
}

static void handle_write_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = container_of(vio, struct pooled_vio, vio);
	struct block_map_zone *zone = pooled->context;

	record_metadata_io_error(vio);
	enter_zone_read_only_mode(zone, result);
	return_to_pool(zone, pooled);
}

static void write_page_endio(struct bio *bio);

static void write_initialized_page(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = container_of(vio, struct pooled_vio, vio);
	struct block_map_zone *zone = pooled->context;
	struct tree_page *tree_page = completion->parent;
	struct block_map_page *page = (struct block_map_page *) vio->data;
	unsigned int operation = REQ_OP_WRITE | REQ_PRIO;

	/*
	 * Now that we know the page has been written at least once, mark the copy we are writing
	 * as initialized.
	 */
	page->header.initialized = true;

	if (zone->flusher == tree_page)
		operation |= REQ_PREFLUSH;

	submit_metadata_vio(vio,
			    vdo_get_block_map_page_pbn(page),
			    write_page_endio,
			    handle_write_error,
			    operation);
}

static void write_page_endio(struct bio *bio)
{
	struct pooled_vio *vio = bio->bi_private;
	struct block_map_zone *zone = vio->context;
	struct block_map_page *page = (struct block_map_page *) vio->vio.data;

	continue_vio_after_io(&vio->vio,
			      (page->header.initialized ?
			       finish_page_write :
			       write_initialized_page),
			      zone->thread_id);
}

static void write_page(struct tree_page *tree_page, struct pooled_vio *vio)
{
	struct vdo_completion *completion = &vio->vio.completion;
	struct block_map_zone *zone = vio->context;
	struct block_map_page *page = vdo_as_block_map_page(tree_page);

	if ((zone->flusher != tree_page) &&
	    is_not_older(zone, tree_page->generation, zone->generation)) {
		/*
		 * This page was re-dirtied after the last flush was issued, hence we need to do
		 * another flush.
		 */
		enqueue_page(tree_page, zone);
		return_to_pool(zone, vio);
		return;
	}

	completion->parent = tree_page;
	memcpy(vio->vio.data, tree_page->page_buffer, VDO_BLOCK_SIZE);
	completion->callback_thread_id = zone->thread_id;

	tree_page->writing = true;
	tree_page->writing_generation = tree_page->generation;
	tree_page->writing_recovery_lock = tree_page->recovery_lock;

	/* Clear this now so that we know this page is not on any dirty list. */
	tree_page->recovery_lock = 0;

	/*
	 * We've already copied the page into the vio which will write it, so if it was not yet
	 * initialized, the first write will indicate that (for torn write protection). It is now
	 * safe to mark it as initialized in memory since if the write fails, the in memory state
	 * will become irrelevant.
	 */
	if (page->header.initialized) {
		write_initialized_page(completion);
		return;
	}

	page->header.initialized = true;
	submit_metadata_vio(&vio->vio,
			    vdo_get_block_map_page_pbn(page),
			    write_page_endio,
			    handle_write_error,
			    REQ_OP_WRITE | REQ_PRIO);
}

/*
 * Schedule a batch of dirty pages for writing.
 *
 * Implements vdo_dirty_callback.
 */
static void write_dirty_pages_callback(struct list_head *expired, void *context)
{
	struct tree_page *page, *tmp;
	struct block_map_zone *zone = (struct block_map_zone *) context;
	u8 generation = zone->generation;

	list_for_each_entry_safe(page, tmp, expired, entry) {
		int result;

		list_del_init(&page->entry);

		result = ASSERT(!is_waiting(&page->waiter),
				"Newly expired page not already waiting to write");
		if (result != VDO_SUCCESS) {
			enter_zone_read_only_mode(zone, result);
			continue;
		}

		set_generation(zone, page, generation);
		if (!page->writing)
			enqueue_page(page, zone);
	}
}

/* Release a lock on a page which was being loaded or allocated. */
static void release_page_lock(struct data_vio *data_vio, char *what)
{
	struct block_map_zone *zone;
	struct tree_lock *lock_holder;
	struct tree_lock *lock = &data_vio->tree_lock;

	ASSERT_LOG_ONLY(lock->locked,
			"release of unlocked block map page %s for key %llu in tree %u",
			what, (unsigned long long) lock->key,
			lock->root_index);

	zone = get_block_map_zone(data_vio);
	lock_holder = int_map_remove(zone->loading_pages, lock->key);
	ASSERT_LOG_ONLY((lock_holder == lock),
			"block map page %s mismatch for key %llu in tree %u",
			what,
			(unsigned long long) lock->key,
			lock->root_index);
	lock->locked = false;
}

static void finish_lookup(struct data_vio *data_vio, int result)
{
	struct block_map_zone *zone;

	data_vio->tree_lock.height = 0;

	zone = get_block_map_zone(data_vio);
	--zone->active_lookups;

	set_data_vio_logical_callback(data_vio, continue_data_vio_with_block_map_slot);
	data_vio->vio.completion.error_handler = handle_data_vio_error;
	continue_data_vio_with_error(data_vio, result);
}

static void abort_lookup_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	int result = *((int *) context);

	if (!data_vio->write) {
		if (result == VDO_NO_SPACE)
			result = VDO_SUCCESS;
	} else if (result != VDO_NO_SPACE) {
		result = VDO_READ_ONLY;
	}

	finish_lookup(data_vio, result);
}

static void abort_lookup(struct data_vio *data_vio, int result, char *what)
{
	if (result != VDO_NO_SPACE)
		enter_zone_read_only_mode(get_block_map_zone(data_vio), result);

	if (data_vio->tree_lock.locked) {
		release_page_lock(data_vio, what);
		notify_all_waiters(&data_vio->tree_lock.waiters, abort_lookup_for_waiter, &result);
	}

	finish_lookup(data_vio, result);
}

static void abort_load(struct data_vio *data_vio, int result)
{
	abort_lookup(data_vio, result, "load");
}

static bool __must_check
is_invalid_tree_entry(const struct vdo *vdo, const struct data_location *mapping, height_t height)
{
	if (!vdo_is_valid_location(mapping) ||
	    vdo_is_state_compressed(mapping->state) ||
	    (vdo_is_mapped_location(mapping) && (mapping->pbn == VDO_ZERO_BLOCK)))
		return true;

	/* Roots aren't physical data blocks, so we can't check their PBNs. */
	if (height == VDO_BLOCK_MAP_TREE_HEIGHT)
		return false;

	return !vdo_is_physical_data_block(vdo->depot, mapping->pbn);
}

static void load_block_map_page(struct block_map_zone *zone, struct data_vio *data_vio);
static void allocate_block_map_page(struct block_map_zone *zone, struct data_vio *data_vio);

static void continue_with_loaded_page(struct data_vio *data_vio, struct block_map_page *page)
{
	struct tree_lock *lock = &data_vio->tree_lock;
	struct block_map_tree_slot slot = lock->tree_slots[lock->height];
	struct data_location mapping =
		vdo_unpack_block_map_entry(&page->entries[slot.block_map_slot.slot]);

	if (is_invalid_tree_entry(vdo_from_data_vio(data_vio), &mapping, lock->height)) {
		uds_log_error_strerror(VDO_BAD_MAPPING,
				       "Invalid block map tree PBN: %llu with state %u for page index %u at height %u",
				       (unsigned long long) mapping.pbn,
				       mapping.state,
				       lock->tree_slots[lock->height - 1].page_index,
				       lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!vdo_is_mapped_location(&mapping)) {
		/* The page we need is unallocated */
		allocate_block_map_page(get_block_map_zone(data_vio), data_vio);
		return;
	}

	lock->tree_slots[lock->height - 1].block_map_slot.pbn = mapping.pbn;
	if (lock->height == 1) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	/* We know what page we need to load next */
	load_block_map_page(get_block_map_zone(data_vio), data_vio);
}

static void continue_load_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);

	data_vio->tree_lock.height--;
	continue_with_loaded_page(data_vio, (struct block_map_page *) context);
}

static void finish_block_map_page_load(struct vdo_completion *completion)
{
	physical_block_number_t pbn;
	struct tree_page *tree_page;
	struct block_map_page *page;
	nonce_t nonce;
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = vio_as_pooled_vio(vio);
	struct data_vio *data_vio = completion->parent;
	struct block_map_zone *zone = pooled->context;
	struct tree_lock *tree_lock = &data_vio->tree_lock;

	tree_lock->height--;
	pbn = tree_lock->tree_slots[tree_lock->height].block_map_slot.pbn;
	tree_page = get_tree_page(zone, tree_lock);
	page = (struct block_map_page *) tree_page->page_buffer;
	nonce = zone->block_map->nonce;

	if (!vdo_copy_valid_page(vio->data, nonce, pbn, page))
		vdo_format_block_map_page(page, nonce, pbn, false);
	return_vio_to_pool(zone->vio_pool, pooled);

	/* Release our claim to the load and wake any waiters */
	release_page_lock(data_vio, "load");
	notify_all_waiters(&tree_lock->waiters, continue_load_for_waiter, page);
	continue_with_loaded_page(data_vio, page);
}

static void handle_io_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio *vio = as_vio(completion);
	struct pooled_vio *pooled = container_of(vio, struct pooled_vio, vio);
	struct data_vio *data_vio = completion->parent;
	struct block_map_zone *zone = pooled->context;

	record_metadata_io_error(vio);
	return_vio_to_pool(zone->vio_pool, pooled);
	abort_load(data_vio, result);
}

static void load_page_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct data_vio *data_vio = vio->completion.parent;

	continue_vio_after_io(vio, finish_block_map_page_load, data_vio->logical.zone->thread_id);
}

static void load_page(struct waiter *waiter, void *context)
{
	struct pooled_vio *pooled = context;
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct tree_lock *lock = &data_vio->tree_lock;
	physical_block_number_t pbn = lock->tree_slots[lock->height - 1].block_map_slot.pbn;

	pooled->vio.completion.parent = data_vio;
	submit_metadata_vio(&pooled->vio,
			    pbn,
			    load_page_endio,
			    handle_io_error,
			    REQ_OP_READ | REQ_PRIO);
}

/*
 * If the page is already locked, queue up to wait for the lock to be released. If the lock is
 * acquired, @data_vio->tree_lock.locked will be true.
 */
static int attempt_page_lock(struct block_map_zone *zone, struct data_vio *data_vio)
{
	int result;
	struct tree_lock *lock_holder;
	struct tree_lock *lock = &data_vio->tree_lock;
	height_t height = lock->height;
	struct block_map_tree_slot tree_slot = lock->tree_slots[height];
	union page_key key;

	key.descriptor = (struct page_descriptor) {
		.root_index = lock->root_index,
		.height = height,
		.page_index = tree_slot.page_index,
		.slot = tree_slot.block_map_slot.slot,
	};
	lock->key = key.key;

	result = int_map_put(zone->loading_pages, lock->key, lock, false, (void **) &lock_holder);
	if (result != VDO_SUCCESS)
		return result;

	if (lock_holder == NULL) {
		/* We got the lock */
		data_vio->tree_lock.locked = true;
		return VDO_SUCCESS;
	}

	/* Someone else is loading or allocating the page we need */
	enqueue_waiter(&lock_holder->waiters, &data_vio->waiter);
	return VDO_SUCCESS;
}

/* Load a block map tree page from disk, for the next level in the data vio tree lock. */
static void load_block_map_page(struct block_map_zone *zone, struct data_vio *data_vio)
{
	int result;

	result = attempt_page_lock(zone, data_vio);
	if (result != VDO_SUCCESS) {
		abort_load(data_vio, result);
		return;
	}

	if (data_vio->tree_lock.locked) {
		data_vio->waiter.callback = load_page;
		acquire_vio_from_pool(zone->vio_pool, &data_vio->waiter);
	}
}

static void abort_allocation(struct data_vio *data_vio, int result)
{
	abort_lookup(data_vio, result, "allocation");
}

static void allocation_failure(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if (vdo_get_callback_thread_id() != data_vio->logical.zone->thread_id) {
		launch_data_vio_logical_callback(data_vio, allocation_failure);
		return;
	}

	abort_allocation(data_vio, completion->result);
}

static void continue_allocation_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	physical_block_number_t pbn = *((physical_block_number_t *) context);

	tree_lock->height--;
	data_vio->tree_lock.tree_slots[tree_lock->height].block_map_slot.pbn = pbn;

	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(get_block_map_zone(data_vio), data_vio);
}

/*
 * Record the allocation in the tree and wake any waiters now that the write lock has been
 * released.
 */
static void finish_block_map_allocation(struct vdo_completion *completion)
{
	physical_block_number_t pbn;
	struct tree_page *tree_page;
	struct block_map_page *page;
	sequence_number_t old_lock;
	struct data_vio *data_vio = as_data_vio(completion);
	struct block_map_zone *zone = get_block_map_zone(data_vio);
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	height_t height = tree_lock->height;

	assert_data_vio_in_logical_zone(data_vio);

	tree_page = get_tree_page(zone, tree_lock);
	pbn = tree_lock->tree_slots[height - 1].block_map_slot.pbn;

	/* Record the allocation. */
	page = (struct block_map_page *) tree_page->page_buffer;
	old_lock = tree_page->recovery_lock;
	vdo_update_block_map_page(page,
				  data_vio,
				  pbn,
				  VDO_MAPPING_STATE_UNCOMPRESSED,
				  &tree_page->recovery_lock);

	if (is_waiting(&tree_page->waiter)) {
		/* This page is waiting to be written out. */
		if (zone->flusher != tree_page)
			/*
			 * The outstanding flush won't cover the update we just made, so mark the
			 * page as needing another flush.
			 */
			set_generation(zone, tree_page, zone->generation);
	} else {
		/* Put the page on a dirty list */
		if (old_lock == 0)
			INIT_LIST_HEAD(&tree_page->entry);
		vdo_add_to_dirty_lists(zone->dirty_lists,
				       &tree_page->entry,
				       old_lock,
				       tree_page->recovery_lock);
	}

	tree_lock->height--;
	if (height > 1) {
		/* Format the interior node we just allocated (in memory). */
		tree_page = get_tree_page(zone, tree_lock);
		vdo_format_block_map_page(tree_page->page_buffer,
					  zone->block_map->nonce,
					  pbn,
					  false);
	}

	/* Release our claim to the allocation and wake any waiters */
	release_page_lock(data_vio, "allocation");
	notify_all_waiters(&tree_lock->waiters, continue_allocation_for_waiter, &pbn);
	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(zone, data_vio);
}

static void release_block_map_write_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);

	release_data_vio_allocation_lock(data_vio, true);
	launch_data_vio_logical_callback(data_vio, finish_block_map_allocation);
}

/*
 * Newly allocated block map pages are set to have to MAXIMUM_REFERENCES after they are journaled,
 * to prevent deduplication against the block after we release the write lock on it, but before we
 * write out the page.
 */
static void set_block_map_page_reference_count(struct vdo_completion *completion)
{
	physical_block_number_t pbn;
	struct data_vio *data_vio = as_data_vio(completion);
	struct tree_lock *lock = &data_vio->tree_lock;

	assert_data_vio_in_allocated_zone(data_vio);

	pbn = lock->tree_slots[lock->height - 1].block_map_slot.pbn;
	completion->callback = release_block_map_write_lock;
	vdo_add_slab_journal_entry(vdo_get_slab(completion->vdo->depot, pbn)->journal, data_vio);
}

static void journal_block_map_allocation(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	set_data_vio_allocated_zone_callback(data_vio, set_block_map_page_reference_count);
	vdo_add_recovery_journal_entry(completion->vdo->recovery_journal, data_vio);
}

static void allocate_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct tree_lock *lock = &data_vio->tree_lock;
	physical_block_number_t pbn;

	assert_data_vio_in_allocated_zone(data_vio);

	if (!vdo_allocate_block_in_zone(data_vio))
		return;

	pbn = data_vio->allocation.pbn;
	lock->tree_slots[lock->height - 1].block_map_slot.pbn = pbn;
	vdo_set_up_reference_operation_with_lock(VDO_JOURNAL_BLOCK_MAP_INCREMENT,
						 pbn,
						 VDO_MAPPING_STATE_UNCOMPRESSED,
						 data_vio->allocation.lock,
						 &data_vio->operation);
	launch_data_vio_journal_callback(data_vio, journal_block_map_allocation);
}

static void allocate_block_map_page(struct block_map_zone *zone, struct data_vio *data_vio)
{
	int result;

	if (!data_vio->write || data_vio->is_trim) {
		/* This is a pure read or a trim, so there's nothing left to do here. */
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	result = attempt_page_lock(zone, data_vio);
	if (result != VDO_SUCCESS) {
		abort_allocation(data_vio, result);
		return;
	}

	if (!data_vio->tree_lock.locked)
		return;

	data_vio_allocate_data_block(data_vio,
				     VIO_BLOCK_MAP_WRITE_LOCK,
				     allocate_block,
				     allocation_failure);
}

/*
 * vdo_find_block_map_slot(): Find the block map slot in which the block map entry for a data_vio
 *                            resides and cache that result in the data_vio.
 * @data_vio: The data_vio
 *
 * All ancestors in the tree will be allocated or loaded, as needed.
 */
void vdo_find_block_map_slot(struct data_vio *data_vio)
{
	page_number_t page_index;
	struct block_map_tree_slot tree_slot;
	struct data_location mapping;
	struct block_map_page *page = NULL;
	struct tree_lock *lock = &data_vio->tree_lock;
	struct block_map_zone *zone = get_block_map_zone(data_vio);

	zone->active_lookups++;
	if (vdo_is_state_draining(&zone->state)) {
		finish_lookup(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	lock->tree_slots[0].block_map_slot.slot =
		data_vio->logical.lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
	page_index = (lock->tree_slots[0].page_index / zone->block_map->root_count);
	tree_slot = (struct block_map_tree_slot) {
		.page_index = page_index / VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		.block_map_slot = {
			.pbn = 0,
			.slot = page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		},
	};

	for (lock->height = 1; lock->height <= VDO_BLOCK_MAP_TREE_HEIGHT; lock->height++) {
		physical_block_number_t pbn;

		lock->tree_slots[lock->height] = tree_slot;
		page = (struct block_map_page *) (get_tree_page(zone, lock)->page_buffer);
		pbn = vdo_get_block_map_page_pbn(page);
		if (pbn != VDO_ZERO_BLOCK) {
			lock->tree_slots[lock->height].block_map_slot.pbn = pbn;
			break;
		}

		/* Calculate the index and slot for the next level. */
		tree_slot.block_map_slot.slot =
			tree_slot.page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
		tree_slot.page_index = tree_slot.page_index / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
	}

	/* The page at this height has been allocated and loaded. */
	mapping = vdo_unpack_block_map_entry(&page->entries[tree_slot.block_map_slot.slot]);
	if (is_invalid_tree_entry(vdo_from_data_vio(data_vio), &mapping, lock->height)) {
		uds_log_error_strerror(VDO_BAD_MAPPING,
				       "Invalid block map tree PBN: %llu with state %u for page index %u at height %u",
				       (unsigned long long) mapping.pbn,
				       mapping.state,
				       lock->tree_slots[lock->height - 1].page_index,
				       lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!vdo_is_mapped_location(&mapping)) {
		/* The page we want one level down has not been allocated, so allocate it. */
		allocate_block_map_page(zone, data_vio);
		return;
	}

	lock->tree_slots[lock->height - 1].block_map_slot.pbn = mapping.pbn;
	if (lock->height == 1) {
		/* This is the ultimate block map page, so we're done */
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	/* We know what page we need to load. */
	load_block_map_page(zone, data_vio);
}

/*
 * Find the PBN of a leaf block map page. This method may only be used after all allocated tree
 * pages have been loaded, otherwise, it may give the wrong answer (0).
 */
physical_block_number_t
vdo_find_block_map_page_pbn(struct block_map *map, page_number_t page_number)
{
	struct data_location mapping;
	struct tree_page *tree_page;
	struct block_map_page *page;
	root_count_t root_index = page_number % map->root_count;
	page_number_t page_index = page_number / map->root_count;
	slot_number_t slot = page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	page_index /= VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	tree_page = vdo_get_tree_page_by_index(map->forest, root_index, 1, page_index);
	page = (struct block_map_page *) tree_page->page_buffer;
	if (!page->header.initialized)
		return VDO_ZERO_BLOCK;

	mapping = vdo_unpack_block_map_entry(&page->entries[slot]);
	if (!vdo_is_valid_location(&mapping) || vdo_is_state_compressed(mapping.state))
		return VDO_ZERO_BLOCK;
	return mapping.pbn;
}

/*
 * Write a tree page or indicate that it has been re-dirtied if it is already being written. This
 * method is used when correcting errors in the tree during read-only rebuild.
 */
void vdo_write_tree_page(struct tree_page *page, struct block_map_zone *zone)
{
	bool waiting = is_waiting(&page->waiter);

	if (waiting && (zone->flusher == page))
		return;

	set_generation(zone, page, zone->generation);
	if (waiting || page->writing)
		return;

	enqueue_page(page, zone);
}
/**
 * DOC: Block map eras
 *
 * The block map era, or maximum age, is used as follows:
 *
 * Each block map page, when dirty, records the earliest recovery journal block sequence number of
 * the changes reflected in that dirty block. Sequence numbers are classified into eras: every
 * @maximum_age sequence numbers, we switch to a new era. Block map pages are assigned to eras
 * according to the sequence number they record.
 *
 * In the current (newest) era, block map pages are not written unless there is cache pressure. In
 * the next oldest era, each time a new journal block is written 1/@maximum_age of the pages in
 * this era are issued for write. In all older eras, pages are issued for write immediately.
 */

/**
 * initialize_block_map_zone() - Initialize the per-zone portions of the block map.
 * @maximum_age: The number of journal blocks before a dirtied page is considered old and must be
 *               written out.
 */
static int __must_check initialize_block_map_zone(struct block_map *map,
						  zone_count_t zone_number,
						  const struct thread_config *thread_config,
						  struct vdo *vdo,
						  struct read_only_notifier *read_only_notifier,
						  page_count_t cache_size,
						  block_count_t maximum_age)
{
	int result;
	struct block_map_zone *zone = &map->zones[zone_number];

	STATIC_ASSERT_SIZEOF(struct page_descriptor, sizeof(u64));

	zone->zone_number = zone_number;
	zone->thread_id = vdo_get_logical_zone_thread(thread_config, zone_number);
	zone->block_map = map;
	zone->read_only_notifier = read_only_notifier;
	result = vdo_make_dirty_lists(maximum_age,
				      write_dirty_pages_callback,
				      zone,
				      &zone->dirty_lists);
	if (result != VDO_SUCCESS)
		return result;

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0, &zone->loading_pages);
	if (result != VDO_SUCCESS)
		return result;

	result = make_vio_pool(vdo,
			       BLOCK_MAP_VIO_POOL_SIZE,
			       zone->thread_id,
			       VIO_TYPE_BLOCK_MAP_INTERIOR,
			       VIO_PRIORITY_METADATA,
			       zone,
			       &zone->vio_pool);
	if (result != VDO_SUCCESS)
		return result;

	vdo_set_admin_state_code(&zone->state, VDO_ADMIN_STATE_NORMAL_OPERATION);

	return vdo_make_page_cache(vdo,
				   cache_size / map->zone_count,
				   maximum_age,
				   zone,
				   &zone->page_cache);
}

/* Implements vdo_zone_thread_getter */
static thread_id_t get_block_map_zone_thread_id(void *context, zone_count_t zone_number)
{
	struct block_map *map = context;

	return map->zones[zone_number].thread_id;
}

/* Implements vdo_action_preamble */
static void prepare_for_era_advance(void *context, struct vdo_completion *parent)
{
	struct block_map *map = context;

	map->current_era_point = map->pending_era_point;
	vdo_complete_completion(parent);
}

/* Implements vdo_zone_action */
static void advance_block_map_zone_era(void *context,
				       zone_count_t zone_number,
				       struct vdo_completion *parent)
{
	struct block_map *map = context;
	struct block_map_zone *zone = &map->zones[zone_number];

	vdo_advance_page_cache_period(zone->page_cache, map->current_era_point);
	vdo_advance_dirty_lists_period(zone->dirty_lists, map->current_era_point);
	vdo_finish_completion(parent, VDO_SUCCESS);
}

/*
 * Schedule an era advance if necessary. This method should not be called directly. Rather, call
 * vdo_schedule_default_action() on the block map's action manager.
 *
 * Implements vdo_action_scheduler.
 */
static bool schedule_era_advance(void *context)
{
	struct block_map *map = context;

	if (map->current_era_point == map->pending_era_point)
		return false;

	return vdo_schedule_action(map->action_manager,
				   prepare_for_era_advance,
				   advance_block_map_zone_era,
				   NULL,
				   NULL);
}

static void uninitialize_block_map_zone(struct block_map_zone *zone)
{
	UDS_FREE(UDS_FORGET(zone->dirty_lists));
	free_vio_pool(UDS_FORGET(zone->vio_pool));
	free_int_map(UDS_FORGET(zone->loading_pages));
	vdo_free_page_cache(UDS_FORGET(zone->page_cache));
}

void vdo_free_block_map(struct block_map *map)
{
	zone_count_t zone;

	if (map == NULL)
		return;

	for (zone = 0; zone < map->zone_count; zone++)
		uninitialize_block_map_zone(&map->zones[zone]);

	vdo_abandon_block_map_growth(map);
	vdo_free_forest(UDS_FORGET(map->forest));
	UDS_FREE(UDS_FORGET(map->action_manager));
	UDS_FREE(map);
}

/* @journal may be NULL. */
int vdo_decode_block_map(struct block_map_state_2_0 state,
			 block_count_t logical_blocks,
			 const struct thread_config *thread_config,
			 struct vdo *vdo,
			 struct read_only_notifier *read_only_notifier,
			 struct recovery_journal *journal,
			 nonce_t nonce,
			 page_count_t cache_size,
			 block_count_t maximum_age,
			 struct block_map **map_ptr)
{
	struct block_map *map;
	int result;
	zone_count_t zone = 0;

	STATIC_ASSERT(VDO_BLOCK_MAP_ENTRIES_PER_PAGE ==
		      ((VDO_BLOCK_SIZE - sizeof(struct block_map_page)) /
		       sizeof(struct block_map_entry)));
	result = ASSERT(cache_size > 0, "block map cache size is specified");
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE_EXTENDED(struct block_map,
				       thread_config->logical_zone_count,
				       struct block_map_zone,
				       __func__,
				       &map);
	if (result != UDS_SUCCESS)
		return result;

	map->root_origin = state.root_origin;
	map->root_count = state.root_count;
	map->entry_count = logical_blocks;
	map->journal = journal;
	map->nonce = nonce;

	result = vdo_make_forest(map, map->entry_count);
	if (result != VDO_SUCCESS) {
		vdo_free_block_map(map);
		return result;
	}

	vdo_replace_forest(map);

	map->zone_count = thread_config->logical_zone_count;
	for (zone = 0; zone < map->zone_count; zone++) {
		result = initialize_block_map_zone(map,
						   zone,
						   thread_config,
						   vdo,
						   read_only_notifier,
						   cache_size,
						   maximum_age);
		if (result != VDO_SUCCESS) {
			vdo_free_block_map(map);
			return result;
		}
	}

	result = vdo_make_action_manager(map->zone_count,
					 get_block_map_zone_thread_id,
					 vdo_get_recovery_journal_thread_id(journal),
					 map,
					 schedule_era_advance,
					 vdo,
					 &map->action_manager);
	if (result != VDO_SUCCESS) {
		vdo_free_block_map(map);
		return result;
	}

	*map_ptr = map;
	return VDO_SUCCESS;
}

struct block_map_state_2_0 vdo_record_block_map(const struct block_map *map)
{
	return (struct block_map_state_2_0) {
		.flat_page_origin = VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
		/* This is the flat page count, which has turned out to always be 0. */
		.flat_page_count = 0,
		.root_origin = map->root_origin,
		.root_count = map->root_count,
	};
}

/* The block map needs to know the journals' sequence number to initialize the eras. */
void vdo_initialize_block_map_from_journal(struct block_map *map, struct recovery_journal *journal)
{
	zone_count_t z = 0;

	map->current_era_point = vdo_get_recovery_journal_current_sequence_number(journal);
	map->pending_era_point = map->current_era_point;

	for (z = 0; z < map->zone_count; z++) {
		struct block_map_zone *zone = &map->zones[z];

		vdo_set_dirty_lists_current_period(zone->dirty_lists, map->current_era_point);
		vdo_set_page_cache_initial_period(zone->page_cache, map->current_era_point);
	}
}

/* Compute the logical zone for the LBN of a data vio. */
zone_count_t vdo_compute_logical_zone(struct data_vio *data_vio)
{
	struct block_map *map = vdo_from_data_vio(data_vio)->block_map;
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	page_number_t page_number = data_vio->logical.lbn / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	tree_lock->tree_slots[0].page_index = page_number;
	tree_lock->root_index = page_number % map->root_count;
	return (tree_lock->root_index % map->zone_count);
}

/**
 * vdo_find_block_map_slot() - Compute the block map slot in which the block map entry for a
 *                             data_vio resides and cache that in the data_vio.
 * @thread_id: The thread on which to run the callback.
 * Update the block map era information for a newly finished journal block.
 * This method must be called from the journal zone thread.
 */
void vdo_advance_block_map_era(struct block_map *map, sequence_number_t recovery_block_number)
{
	if (map == NULL)
		return;

	map->pending_era_point = recovery_block_number;
	vdo_schedule_default_action(map->action_manager);
}

/* Implements vdo_admin_initiator */
static void initiate_drain(struct admin_state *state)
{
	struct block_map_zone *zone = container_of(state, struct block_map_zone, state);

	ASSERT_LOG_ONLY((zone->active_lookups == 0),
			"%s() called with no active lookups",
			__func__);

	if (!vdo_is_state_suspending(state))
		vdo_flush_dirty_lists(zone->dirty_lists);

	vdo_drain_page_cache(zone->page_cache);
	vdo_block_map_check_for_drain_complete(zone);
}

/* Implements vdo_zone_action. */
static void drain_zone(void *context, zone_count_t zone_number, struct vdo_completion *parent)
{
	struct block_map *map = context;
	struct block_map_zone *zone = &map->zones[zone_number];

	vdo_start_draining(&zone->state,
			   vdo_get_current_manager_operation(map->action_manager),
			   parent,
			   initiate_drain);
}

void vdo_drain_block_map(struct block_map *map,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager, operation, NULL, drain_zone, NULL, parent);
}

/* Implements vdo_zone_action. */
static void
resume_block_map_zone(void *context, zone_count_t zone_number, struct vdo_completion *parent)
{
	struct block_map *map = context;
	struct block_map_zone *zone = &map->zones[zone_number];

	vdo_finish_completion(parent, vdo_resume_if_quiescent(&zone->state));
}

void vdo_resume_block_map(struct block_map *map, struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager,
			       VDO_ADMIN_STATE_RESUMING,
			       NULL,
			       resume_block_map_zone,
			       NULL,
			       parent);
}

/* Allocate an expanded collection of trees, for a future growth. */
int vdo_prepare_to_grow_block_map(struct block_map *map, block_count_t new_logical_blocks)
{
	if (map->next_entry_count == new_logical_blocks)
		return VDO_SUCCESS;

	if (map->next_entry_count > 0)
		vdo_abandon_block_map_growth(map);

	if (new_logical_blocks < map->entry_count) {
		map->next_entry_count = map->entry_count;
		return VDO_SUCCESS;
	}

	return vdo_make_forest(map, new_logical_blocks);
}

/* Implements vdo_action_preamble */
static void grow_forest(void *context, struct vdo_completion *completion)
{
	vdo_replace_forest(context);
	vdo_complete_completion(completion);
}

/* Requires vdo_prepare_to_grow_block_map() to have been previously called. */
void vdo_grow_block_map(struct block_map *map, struct vdo_completion *parent)
{
	vdo_schedule_operation(map->action_manager,
			       VDO_ADMIN_STATE_SUSPENDED_OPERATION,
			       grow_forest,
			       NULL,
			       NULL,
			       parent);
}

void vdo_abandon_block_map_growth(struct block_map *map)
{
	vdo_abandon_forest(map);
}

/* Release the page completion and then continue the requester. */
static inline void finish_processing_page(struct vdo_completion *completion, int result)
{
	struct vdo_completion *parent = completion->parent;

	vdo_release_page_completion(completion);
	vdo_continue_completion(parent, result);
}

static void handle_page_error(struct vdo_completion *completion)
{
	finish_processing_page(completion, completion->result);
}

/* Fetch the mapping page for a block map update, and call the provided handler when fetched. */
static void fetch_mapping_page(struct data_vio *data_vio, bool modifiable, vdo_action *action)
{
	struct block_map_zone *zone = data_vio->logical.zone->block_map_zone;

	if (vdo_is_state_draining(&zone->state)) {
		continue_data_vio_with_error(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	vdo_init_page_completion(&data_vio->page_completion,
				 zone->page_cache,
				 data_vio->tree_lock.tree_slots[0].block_map_slot.pbn,
				 modifiable,
				 &data_vio->vio.completion,
				 action,
				 handle_page_error);
	vdo_get_page(&data_vio->page_completion.completion);
}

/**
 * clear_mapped_location() - Clear a data_vio's mapped block location, setting it to be unmapped.
 * @data_vio: The data_vio whose mapped block location is to be reset.
 *
 * This indicates the block map entry for the logical block is either unmapped or corrupted.
 */
static void clear_mapped_location(struct data_vio *data_vio)
{
	data_vio->mapped = (struct zoned_pbn) {
		.state = VDO_MAPPING_STATE_UNMAPPED,
	};
}

/**
 * set_mapped_location() - Decode and validate a block map entry, and set the mapped location of a
 *                         data_vio.
 *
 * Return: VDO_SUCCESS or VDO_BAD_MAPPING if the map entry is invalid or an error code for any
 *         other failure
 */
static int __must_check
set_mapped_location(struct data_vio *data_vio, const struct block_map_entry *entry)
{
	/* Unpack the PBN for logging purposes even if the entry is invalid. */
	struct data_location mapped = vdo_unpack_block_map_entry(entry);

	if (vdo_is_valid_location(&mapped)) {
		int result;

		result = vdo_get_physical_zone(vdo_from_data_vio(data_vio),
					       mapped.pbn,
					       &data_vio->mapped.zone);
		if (result == VDO_SUCCESS) {
			data_vio->mapped.pbn = mapped.pbn;
			data_vio->mapped.state = mapped.state;
			return VDO_SUCCESS;
		}

		/*
		 * Return all errors not specifically known to be errors from validating the
		 * location.
		 */
		if ((result != VDO_OUT_OF_RANGE) && (result != VDO_BAD_MAPPING))
			return result;
	}

	/*
	 * Log the corruption even if we wind up ignoring it for write VIOs, converting all cases
	 * to VDO_BAD_MAPPING.
	 */
	uds_log_error_strerror(VDO_BAD_MAPPING,
			       "PBN %llu with state %u read from the block map was invalid",
			       (unsigned long long) mapped.pbn,
			       mapped.state);

	/*
	 * A read VIO has no option but to report the bad mapping--reading zeros would be hiding
	 * known data loss.
	 */
	if (!data_vio->write)
		return VDO_BAD_MAPPING;

	/*
	 * A write VIO only reads this mapping to decref the old block. Treat this as an unmapped
	 * entry rather than fail the write.
	 */
	clear_mapped_location(data_vio);
	return VDO_SUCCESS;
}

/* This callback is registered in vdo_get_mapped_block(). */
static void get_mapping_from_fetched_page(struct vdo_completion *completion)
{
	int result;
	const struct block_map_page *page;
	const struct block_map_entry *entry;
	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_tree_slot *tree_slot;

	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	page = vdo_dereference_readable_page(completion);
	result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	tree_slot = &data_vio->tree_lock.tree_slots[0];
	entry = &page->entries[tree_slot->block_map_slot.slot];

	result = set_mapped_location(data_vio, entry);
	finish_processing_page(completion, result);
}

void vdo_update_block_map_page(struct block_map_page *page,
			       struct data_vio *data_vio,
			       physical_block_number_t pbn,
			       enum block_mapping_state mapping_state,
			       sequence_number_t *recovery_lock)
{
	struct block_map_zone *zone = data_vio->logical.zone->block_map_zone;
	struct block_map *block_map = zone->block_map;
	struct recovery_journal *journal = block_map->journal;
	sequence_number_t old_locked, new_locked;
	struct tree_lock *tree_lock = &data_vio->tree_lock;

	/* Encode the new mapping. */
	page->entries[tree_lock->tree_slots[tree_lock->height].block_map_slot.slot] =
		vdo_pack_block_map_entry(pbn, mapping_state);

	/* Adjust references on the recovery journal blocks. */
	old_locked = *recovery_lock;
	new_locked = data_vio->recovery_sequence_number;

	if ((old_locked == 0) || (old_locked > new_locked)) {
		vdo_acquire_recovery_journal_block_reference(journal,
							     new_locked,
							     VDO_ZONE_TYPE_LOGICAL,
							     zone->zone_number);

		if (old_locked > 0)
			vdo_release_recovery_journal_block_reference(journal,
								     old_locked,
								     VDO_ZONE_TYPE_LOGICAL,
								     zone->zone_number);

		*recovery_lock = new_locked;
	}

	/*
	 * FIXME: explain this more
	 * Release the transferred lock from the data_vio.
	 */
	vdo_release_journal_entry_lock(journal, new_locked);
	data_vio->recovery_sequence_number = 0;
}

static void put_mapping_in_fetched_page(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion->parent);
	struct block_map_page *page;
	sequence_number_t *recovery_lock;
	sequence_number_t old_lock;
	int result;

	if (completion->result != VDO_SUCCESS) {
		finish_processing_page(completion, completion->result);
		return;
	}

	page = vdo_dereference_writable_page(completion);
	result = ASSERT(page != NULL, "page available");
	if (result != VDO_SUCCESS) {
		finish_processing_page(completion, result);
		return;
	}

	recovery_lock = &(as_vdo_page_completion(completion)->info->recovery_lock);
	old_lock = *recovery_lock;
	vdo_update_block_map_page(page,
				  data_vio,
				  data_vio->new_mapped.pbn,
				  data_vio->new_mapped.state,
				  recovery_lock);
	vdo_mark_completed_page_dirty(completion, old_lock, *recovery_lock);
	finish_processing_page(completion, VDO_SUCCESS);
}

/* Read a stored block mapping into a data_vio. */
void vdo_get_mapped_block(struct data_vio *data_vio)
{
	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn == VDO_ZERO_BLOCK) {
		/*
		 * We know that the block map page for this LBN has not been allocated, so the
		 * block must be unmapped.
		 */
		clear_mapped_location(data_vio);
		continue_data_vio(data_vio);
		return;
	}

	fetch_mapping_page(data_vio, false, get_mapping_from_fetched_page);
}

/* Update a stored block mapping to reflect a data_vio's new mapping. */
void vdo_put_mapped_block(struct data_vio *data_vio)
{
	fetch_mapping_page(data_vio, true, put_mapping_in_fetched_page);
}

struct block_map_statistics vdo_get_block_map_statistics(struct block_map *map)
{
	zone_count_t zone = 0;
	struct block_map_statistics totals;

	memset(&totals, 0, sizeof(struct block_map_statistics));

	for (zone = 0; zone < map->zone_count; zone++) {
		struct vdo_page_cache *cache = map->zones[zone].page_cache;
		struct block_map_statistics stats = vdo_get_page_cache_statistics(cache);

		totals.dirty_pages += stats.dirty_pages;
		totals.clean_pages += stats.clean_pages;
		totals.free_pages += stats.free_pages;
		totals.failed_pages += stats.failed_pages;
		totals.incoming_pages += stats.incoming_pages;
		totals.outgoing_pages += stats.outgoing_pages;
		totals.cache_pressure += stats.cache_pressure;
		totals.read_count += stats.read_count;
		totals.write_count += stats.write_count;
		totals.failed_reads += stats.failed_reads;
		totals.failed_writes += stats.failed_writes;
		totals.reclaimed += stats.reclaimed;
		totals.read_outgoing += stats.read_outgoing;
		totals.found_in_cache += stats.found_in_cache;
		totals.discard_required += stats.discard_required;
		totals.wait_for_page += stats.wait_for_page;
		totals.fetch_required += stats.fetch_required;
		totals.pages_loaded += stats.pages_loaded;
		totals.pages_saved += stats.pages_saved;
		totals.flush_count += stats.flush_count;
	}

	return totals;
}
