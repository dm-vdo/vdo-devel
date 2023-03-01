/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SLAB_DEPOT_H
#define SLAB_DEPOT_H

#include <linux/atomic.h>
#include <linux/dm-kcopyd.h>

#include "admin-state.h"
#include "completion.h"
#include "data-vio.h"
#include "priority-table.h"
#include "slab.h"
#include "statistics.h"
#include "types.h"
#include "vdo-component-states.h"
#include "vdo-layout.h"
#include "vio.h"
#include "wait-queue.h"

/*
 * A slab_depot is responsible for managing all of the slabs and block allocators of a VDO. It has
 * a single array of slabs in order to eliminate the need for additional math in order to compute
 * which physical zone a PBN is in. It also has a block_allocator per zone.
 *
 * Load operations are required to be performed on a single thread. Normal operations are assumed
 * to be performed in the appropriate zone. Allocations and reference count updates must be done
 * from the thread of their physical zone. Requests to commit slab journal tail blocks from the
 * recovery journal must be done on the journal zone thread. Save operations are required to be
 * launched from the same thread as the original load operation.
 */

enum {
	/* The number of vios in the vio pool is proportional to the throughput of the VDO. */
	BLOCK_ALLOCATOR_VIO_POOL_SIZE = 128,
};

enum block_allocator_drain_step {
	VDO_DRAIN_ALLOCATOR_START,
	VDO_DRAIN_ALLOCATOR_STEP_SCRUBBER,
	VDO_DRAIN_ALLOCATOR_STEP_SLABS,
	VDO_DRAIN_ALLOCATOR_STEP_SUMMARY,
	VDO_DRAIN_ALLOCATOR_STEP_FINISHED,
};

struct slab_scrubber {
	/* The queue of slabs to scrub first */
	struct list_head high_priority_slabs;
	/* The queue of slabs to scrub once there are no high_priority_slabs */
	struct list_head slabs;
	/* The queue of VIOs waiting for a slab to be scrubbed */
	struct wait_queue waiters;

	/*
	 * The number of slabs that are unrecovered or being scrubbed. This field is modified by
	 * the physical zone thread, but is queried by other threads.
	 */
	slab_count_t slab_count;

	/* The administrative state of the scrubber */
	struct admin_state admin_state;
	/* Whether to only scrub high-priority slabs */
	bool high_priority_only;
	/* The slab currently being scrubbed */
	struct vdo_slab *slab;
	/* The vio for loading slab journal blocks */
	struct vio vio;
};

/* A sub-structure for applying actions in parallel to all an allocator's slabs. */
struct slab_actor {
	/* The number of slabs performing a slab action */
	slab_count_t slab_action_count;
	/* The method to call when a slab action has been completed by all slabs */
	vdo_action *callback;
};

/* A slab_iterator is a structure for iterating over a set of slabs. */
struct slab_iterator {
	struct vdo_slab **slabs;
	struct vdo_slab *next;
	slab_count_t end;
	slab_count_t stride;
};

struct block_allocator {
	struct vdo_completion completion;
	/* The slab depot for this allocator */
	struct slab_depot *depot;
	/* The slab summary zone for this allocator */
	struct slab_summary_zone *summary;
	/* The nonce of the VDO */
	nonce_t nonce;
	/* The physical zone number of this allocator */
	zone_count_t zone_number;
	/* The thread ID for this allocator's physical zone */
	thread_id_t thread_id;
	/* The number of slabs in this allocator */
	slab_count_t slab_count;
	/* The number of the last slab owned by this allocator */
	slab_count_t last_slab;
	/* The reduced priority level used to preserve unopened slabs */
	unsigned int unopened_slab_priority;
	/* The state of this allocator */
	struct admin_state state;
	/* The actor for applying an action to all slabs */
	struct slab_actor slab_actor;

	/* The slab from which blocks are currently being allocated */
	struct vdo_slab *open_slab;
	/* A priority queue containing all slabs available for allocation */
	struct priority_table *prioritized_slabs;
	/* The slab scrubber */
	struct slab_scrubber scrubber;
	/* What phase of the close operation the allocator is to perform */
	enum block_allocator_drain_step drain_step;

	/*
	 * These statistics are all mutated only by the physical zone thread, but are read by other
	 * threads when gathering statistics for the entire depot.
	 */
	/*
	 * The count of allocated blocks in this zone. Not in block_allocator_statistics for
	 * historical reasons.
	 */
	u64 allocated_blocks;
	/* Statistics for this block allocator */
	struct block_allocator_statistics statistics;
	/* Cumulative statistics for the slab journals in this zone */
	struct slab_journal_statistics slab_journal_statistics;
	/* Cumulative statistics for the ref_counts in this zone */
	struct ref_counts_statistics ref_counts_statistics;

	/*
	 * This is the head of a queue of slab journals which have entries in their tail blocks
	 * which have not yet started to commit. When the recovery journal is under space pressure,
	 * slab journals which have uncommitted entries holding a lock on the recovery journal head
	 * are forced to commit their blocks early. This list is kept in order, with the tail
	 * containing the slab journal holding the most recent recovery journal lock.
	 */
	struct list_head dirty_slab_journals;

	/* The vio pool for reading and writing block allocator metadata */
	struct vio_pool *vio_pool;
	/* The dm_kcopyd client for erasing slab journals */
	struct dm_kcopyd_client *eraser;
	/* Iterator over the slabs to be erased */
	struct slab_iterator slabs_to_erase;
};

enum slab_depot_load_type {
	VDO_SLAB_DEPOT_NORMAL_LOAD,
	VDO_SLAB_DEPOT_RECOVERY_LOAD,
	VDO_SLAB_DEPOT_REBUILD_LOAD
};

struct slab_depot {
	zone_count_t zone_count;
	zone_count_t old_zone_count;
	struct vdo *vdo;
	struct slab_config slab_config;
	struct slab_summary *slab_summary;
	struct action_manager *action_manager;

	physical_block_number_t first_block;
	physical_block_number_t last_block;
	physical_block_number_t origin;

	/* slab_size == (1 << slab_size_shift) */
	unsigned int slab_size_shift;

	/* Determines how slabs should be queued during load */
	enum slab_depot_load_type load_type;

	/* The state for notifying slab journals to release recovery journal */
	sequence_number_t active_release_request;
	sequence_number_t new_release_request;

	/* State variables for scrubbing complete handling */
	atomic_t zones_to_scrub;

	/* Array of pointers to individually allocated slabs */
	struct vdo_slab **slabs;
	/* The number of slabs currently allocated and stored in 'slabs' */
	slab_count_t slab_count;

	/* Array of pointers to a larger set of slabs (used during resize) */
	struct vdo_slab **new_slabs;
	/* The number of slabs currently allocated and stored in 'new_slabs' */
	slab_count_t new_slab_count;
	/* The size that 'new_slabs' was allocated for */
	block_count_t new_size;

	/* The last block before resize, for rollback */
	physical_block_number_t old_last_block;
	/* The last block after resize, for resize */
	physical_block_number_t new_last_block;

	/* The block allocators for this depot */
	struct block_allocator allocators[];
};

void vdo_register_slab_for_scrubbing(struct vdo_slab *slab, bool high_priority);

static inline struct block_allocator *vdo_as_block_allocator(struct vdo_completion *completion)
{
	vdo_assert_completion_type(completion->type, VDO_BLOCK_ALLOCATOR_COMPLETION);
	return container_of(completion, struct block_allocator, completion);
}

void vdo_queue_slab(struct vdo_slab *slab);

void vdo_adjust_free_block_count(struct vdo_slab *slab, bool increment);

int __must_check vdo_acquire_provisional_reference(struct vdo_slab *slab,
						   physical_block_number_t pbn,
						   struct pbn_lock *lock);

int __must_check
vdo_allocate_block(struct block_allocator *allocator, physical_block_number_t *block_number_ptr);

int vdo_enqueue_clean_slab_waiter(struct block_allocator *allocator, struct waiter *waiter);

int __must_check vdo_modify_slab_reference_count(struct vdo_slab *slab,
						 const struct journal_point *journal_point,
						 struct reference_updater *updater);

void vdo_release_block_reference(struct block_allocator *allocator,
				 physical_block_number_t pbn,
				 const char *why);

void vdo_notify_slab_journals_are_recovered(struct vdo_completion *completion);

void vdo_dump_block_allocator(const struct block_allocator *allocator);

#ifdef INTERNAL
void scrub_slabs(struct block_allocator *allocator, struct vdo_completion *parent);
int __must_check initialize_slab_scrubber(struct block_allocator *allocator);
int __must_check vdo_prepare_slabs_for_allocation(struct block_allocator *allocator);
void stop_scrubbing(struct block_allocator *allocator);
void vdo_allocate_from_allocator_last_slab(struct block_allocator *allocator);

#endif /* INTERNAL */
int __must_check vdo_decode_slab_depot(struct slab_depot_state_2_0 state,
				       struct vdo *vdo,
				       struct partition *summary_partition,
				       struct slab_depot **depot_ptr);

void vdo_free_slab_depot(struct slab_depot *depot);

struct slab_depot_state_2_0 __must_check vdo_record_slab_depot(const struct slab_depot *depot);

int __must_check vdo_allocate_slab_ref_counts(struct slab_depot *depot);

struct vdo_slab * __must_check
vdo_get_slab(const struct slab_depot *depot, physical_block_number_t pbn);

u8 __must_check vdo_get_increment_limit(struct slab_depot *depot, physical_block_number_t pbn);

bool __must_check
vdo_is_physical_data_block(const struct slab_depot *depot, physical_block_number_t pbn);

block_count_t __must_check vdo_get_slab_depot_allocated_blocks(const struct slab_depot *depot);

block_count_t __must_check vdo_get_slab_depot_data_blocks(const struct slab_depot *depot);

void vdo_get_slab_depot_statistics(const struct slab_depot *depot, struct vdo_statistics *stats);

void vdo_load_slab_depot(struct slab_depot *depot,
			 const struct admin_state_code *operation,
			 struct vdo_completion *parent,
			 void *context);

void vdo_prepare_slab_depot_to_allocate(struct slab_depot *depot,
					enum slab_depot_load_type load_type,
					struct vdo_completion *parent);

void vdo_update_slab_depot_size(struct slab_depot *depot);

int __must_check vdo_prepare_to_grow_slab_depot(struct slab_depot *depot, block_count_t new_size);

void vdo_use_new_slabs(struct slab_depot *depot, struct vdo_completion *parent);

void vdo_abandon_new_slabs(struct slab_depot *depot);

void vdo_drain_slab_depot(struct slab_depot *depot,
			  const struct admin_state_code *operation,
			  struct vdo_completion *parent);

void vdo_resume_slab_depot(struct slab_depot *depot, struct vdo_completion *parent);

void vdo_commit_oldest_slab_journal_tail_blocks(struct slab_depot *depot,
						sequence_number_t recovery_block_number);

void vdo_scrub_all_unrecovered_slabs(struct slab_depot *depot, struct vdo_completion *parent);

void vdo_dump_slab_depot(const struct slab_depot *depot);

#endif /* SLAB_DEPOT_H */
