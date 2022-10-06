// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "data-vio.h"

#include <linux/bio.h>
#include <linux/lz4.h>
#ifdef INTERNAL
#include <linux/minmax.h>
#endif /* INTERNAL */
#include <linux/murmurhash3.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "allocation-selector.h"
#include "bio.h"
#include "block-allocator.h"
#include "block-map.h"
#include "compressed-block.h"
#include "compression-state.h"
#include "dump.h"
#include "int-map.h"
#include "io-submitter.h"
#include "logical-zone.h"
#include "packer.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "status-codes.h"
#include "vdo.h"
#include "vdo-component.h"
#include "vdo-component-states.h"

/**
 * DOC: Bio flags.
 *
 * For certain flags set on user bios, if the user bio has not yet been
 * acknowledged, setting those flags on our own bio(s) for that request may
 * help underlying layers better fulfill the user bio's needs. This constant
 * contains the aggregate of those flags; VDO strips all the other flags, as
 * they convey incorrect information.
 *
 * These flags are always irrelevant if we have already finished the user bio
 * as they are only hints on IO importance. If VDO has finished the user bio,
 * any remaining IO done doesn't care how important finishing the finished bio
 * was.
 *
 * Note that bio.c contains the complete list of flags we believe may be set;
 * the following list explains the action taken with each of those flags VDO
 * could receive:
 *
 * * REQ_SYNC: Passed down if the user bio is not yet completed, since it
 *   indicates the user bio completion is required for further work to be
 *   done by the issuer.
 * * REQ_META: Passed down if the user bio is not yet completed, since it may
 *   mean the lower layer treats it as more urgent, similar to REQ_SYNC.
 * * REQ_PRIO: Passed down if the user bio is not yet completed, since it
 *   indicates the user bio is important.
 * * REQ_NOMERGE: Set only if the incoming bio was split; irrelevant to VDO IO.
 * * REQ_IDLE: Set if the incoming bio had more IO quickly following; VDO's IO
 *   pattern doesn't match incoming IO, so this flag is incorrect for it.
 * * REQ_FUA: Handled separately, and irrelevant to VDO IO otherwise.
 * * REQ_RAHEAD: Passed down, as, for reads, it indicates trivial importance.
 * * REQ_BACKGROUND: Not passed down, as VIOs are a limited resource and VDO
 *   needs them recycled ASAP to service heavy load, which is the only place
 *   where REQ_BACKGROUND might aid in load prioritization.
 */
static unsigned int PASSTHROUGH_FLAGS =
	(REQ_PRIO | REQ_META | REQ_SYNC | REQ_RAHEAD);

static const char *ASYNC_OPERATION_NAMES[] = {
	"launch",
	"acknowledge_write",
	"acquire_hash_lock",
	"attempt_logical_block_lock",
	"lock_duplicate_pbn",
	"check_for_duplication",
	"cleanup",
	"compress_data_vio",
	"find_block_map_slot",
	"get_mapped_block/for_read",
	"get_mapped_block/for_dedupe",
	"get_mapped_block/for_write",
	"hash_data_vio",
	"journal_decrement_for_dedupe",
	"journal_decrement_for_write",
	"journal_increment_for_compression",
	"journal_increment_for_dedupe",
	"journal_increment_for_write",
	"journal_mapping_for_compression",
	"journal_mapping_for_dedupe",
	"journal_mapping_for_write",
	"journal_unmapping_for_dedupe",
	"journal_unmapping_for_write",
	"vdo_attempt_packing",
	"put_mapped_block/for_write",
	"put_mapped_block/for_dedupe",
	"read_data_vio",
	"update_dedupe_index",
	"verify_duplication",
	"write_data_vio",
};

/*
 * The steps taken cleaning up a VIO, in the order they are performed.
 */
enum data_vio_cleanup_stage {
	VIO_CLEANUP_START,
	VIO_RELEASE_ALLOCATED = VIO_CLEANUP_START,
	VIO_RELEASE_RECOVERY_LOCKS,
	VIO_RELEASE_HASH_LOCK,
	VIO_RELEASE_LOGICAL,
	VIO_CLEANUP_DONE
};

/*
 * Actions to take on error used by abort_on_error().
 */
enum read_only_action {
	NOT_READ_ONLY,
	READ_ONLY,
};

void destroy_data_vio(struct data_vio *data_vio)
{
	if (data_vio == NULL)
		return;

	vdo_free_bio(UDS_FORGET(data_vio_as_vio(data_vio)->bio));
	UDS_FREE(UDS_FORGET(data_vio->compression.block));
	UDS_FREE(UDS_FORGET(data_vio->data_block));
	UDS_FREE(UDS_FORGET(data_vio->scratch_block));
}

/**
 * allocate_data_vio_components() - Allocate the components of a data_vio.
 * @data_vio: The data_vio being constructed.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check allocate_data_vio_components(struct data_vio *data_vio)
{
	struct vio *vio;
	int result;

	STATIC_ASSERT(VDO_BLOCK_SIZE <= PAGE_SIZE);
	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "vio data",
				     &data_vio->data_block);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "data_vio data allocation failure");

	vio = data_vio_as_vio(data_vio);
	result = vdo_create_bio(&vio->bio);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "data_vio data bio allocation failure");

	result = uds_allocate_memory(VDO_BLOCK_SIZE,
				     0,
				     "compressed block",
				     &data_vio->compression.block);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "data_vio compressed block allocation failure");

	result = uds_allocate_memory(VDO_BLOCK_SIZE, 0, "vio scratch",
				     &data_vio->scratch_block);
	if (result != VDO_SUCCESS)
		return uds_log_error_strerror(result,
					      "data_vio scratch allocation failure");

	return VDO_SUCCESS;
}

int initialize_data_vio(struct data_vio *data_vio)
{
	int result = allocate_data_vio_components(data_vio);

	if (result != VDO_SUCCESS)
		destroy_data_vio(data_vio);

	return result;
}

/**
 * initialize_lbn_lock() - Initialize the LBN lock of a data_vio.
 * @data_vio: The data_vio to initialize.
 * @lbn: The lbn on which the data_vio will operate.
 *
 * In addition to recording the LBN on which the data_vio will operate, it
 * will also find the logical zone associated with the LBN.
 */
static void initialize_lbn_lock(struct data_vio *data_vio,
				logical_block_number_t lbn)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	zone_count_t zone_number;
	struct lbn_lock *lock = &data_vio->logical;

	lock->lbn = lbn;
	lock->locked = false;
	initialize_wait_queue(&lock->waiters);
	zone_number = vdo_compute_logical_zone(data_vio);
	lock->zone = &vdo->logical_zones->zones[zone_number];
}

void attempt_logical_block_lock(struct vdo_completion *completion);

/**
 * launch_data_vio() - (Re)initialize a data_vio to have a new logical
 *		       block number, keeping the same parent and other
 *		       state and send it on its way.
 * @data_vio: The data_vio to initialize.
 * @lbn: The logical block number of the data_vio.
 * @operation: The operation this data_vio will perform.
 */
void launch_data_vio(struct data_vio *data_vio,
		     logical_block_number_t lbn,
		     enum data_vio_operation operation)
{
	struct vio *vio = data_vio_as_vio(data_vio);
	struct vdo_completion *completion = vio_as_completion(vio);

	/*
	 * Clearing the tree lock must happen before initializing the LBN lock,
	 * which also adds information to the tree lock.
	 */
	memset(&data_vio->tree_lock, 0, sizeof(data_vio->tree_lock));
	initialize_lbn_lock(data_vio, lbn);
	INIT_LIST_HEAD(&data_vio->hash_lock_entry);
	INIT_LIST_HEAD(&data_vio->write_entry);

	memset(&data_vio->allocation, 0, sizeof(data_vio->allocation));

	data_vio->is_duplicate = false;

	memset(&data_vio->record_name, 0, sizeof(data_vio->record_name));
	memset(&data_vio->duplicate, 0, sizeof(data_vio->duplicate));

	data_vio->io_operation = operation;
	data_vio->mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
	if (data_vio->is_partial || (data_vio->remaining_discard == 0))
		/* This is either a write or a partial block discard */
		data_vio->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
	else
		/* This is a full block discard */
		data_vio->new_mapped.state = VDO_MAPPING_STATE_UNMAPPED;

	vdo_reset_completion(completion);
	set_data_vio_logical_callback(data_vio, attempt_logical_block_lock);
	vdo_invoke_completion_callback_with_priority(completion,
						     VDO_DEFAULT_Q_MAP_BIO_PRIORITY);
}

static void update_data_vio_error_stats(struct data_vio *data_vio)
{
	static const char *operations[] = {
		[DATA_VIO_UNSPECIFIED_OPERATION] = "empty",
		[DATA_VIO_READ] = "read",
		[DATA_VIO_WRITE] = "write",
		[DATA_VIO_READ_MODIFY_WRITE] = "read-modify-write",
		[DATA_VIO_READ | DATA_VIO_FUA] = "read+fua",
		[DATA_VIO_WRITE | DATA_VIO_FUA] = "write+fua",
		[DATA_VIO_READ_MODIFY_WRITE | DATA_VIO_FUA] =
			"read-modify-write+fua",
	};

	update_vio_error_stats(data_vio_as_vio(data_vio),
			       "Completing %s vio for LBN %llu with error after %s",
			       operations[data_vio->io_operation],
			       (unsigned long long) data_vio->logical.lbn,
			       get_data_vio_operation_name(data_vio));
}

static void perform_cleanup_stage(struct data_vio *data_vio,
				  enum data_vio_cleanup_stage stage);

/**
 * release_allocated_lock() - Release the PBN lock and/or the reference on the
 *			      allocated block at the end of processing a
 *			      data_vio.
 * @completion: The data_vio.
 */
static void release_allocated_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);
	release_data_vio_allocation_lock(data_vio, false);
	perform_cleanup_stage(data_vio, VIO_RELEASE_RECOVERY_LOCKS);
}

/**
 * release_logical_lock() - Release the logical block lock and flush
 *			    generation lock at the end of processing a
 *			    data_vio.
 * @completion: The data_vio.
 */
static void release_logical_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	vdo_release_logical_block_lock(data_vio);
	vdo_release_flush_generation_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_CLEANUP_DONE);
}

/**
 * clean_hash_lock() - Release the hash lock at the end of processing a
 *		       data_vio.
 * @completion: The data_vio.
 */
static void clean_hash_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	vdo_release_hash_lock(data_vio);
	perform_cleanup_stage(data_vio, VIO_RELEASE_LOGICAL);
}

/**
 * finish_cleanup() - Make some assertions about a data_vio which has finished
 *		      cleaning up.
 * @data_vio: The data_vio which has finished cleaning up.
 *
 * If it is part of a multi-block discard, starts on the next block,
 * otherwise, returns it to the pool.
 */
static void finish_cleanup(struct data_vio *data_vio)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);
	enum data_vio_operation operation;

	ASSERT_LOG_ONLY(data_vio->allocation.lock == NULL,
			"complete data_vio has no allocation lock");
	ASSERT_LOG_ONLY(data_vio->hash_lock == NULL,
			"complete data_vio has no hash lock");
	if ((data_vio->remaining_discard <= VDO_BLOCK_SIZE) ||
	    (completion->result != VDO_SUCCESS)) {
		release_data_vio(data_vio);
		return;
	}

	data_vio->remaining_discard -= min_t(uint32_t,
					     data_vio->remaining_discard,
					     VDO_BLOCK_SIZE - data_vio->offset);
	data_vio->is_partial = (data_vio->remaining_discard < VDO_BLOCK_SIZE);
	data_vio->offset = 0;

	if (data_vio->is_partial)
		operation = DATA_VIO_READ_MODIFY_WRITE;
	else
		operation = DATA_VIO_WRITE;

	if (data_vio->user_bio->bi_opf & REQ_FUA)
		operation |= DATA_VIO_FUA;

	completion->requeue = true;
	launch_data_vio(data_vio, data_vio->logical.lbn + 1, operation);
}

/**
 * perform_cleanup_stage() - Perform the next step in the process of cleaning
 *			     up a data_vio.
 * @data_vio: The data_vio to clean up.
 * @stage: The cleanup stage to perform.
 */
static void perform_cleanup_stage(struct data_vio *data_vio,
				  enum data_vio_cleanup_stage stage)
{
	switch (stage) {
	case VIO_RELEASE_ALLOCATED:
		if (data_vio_has_allocation(data_vio)) {
			launch_data_vio_allocated_zone_callback(data_vio,
								release_allocated_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_RECOVERY_LOCKS:
		if ((data_vio->recovery_sequence_number > 0) &&
		    !vdo_is_or_will_be_read_only(vdo_from_data_vio(data_vio)->read_only_notifier) &&
		    (data_vio_as_completion(data_vio)->result != VDO_READ_ONLY))
			uds_log_warning("VDO not read-only when cleaning data_vio with RJ lock");
		fallthrough;

	case VIO_RELEASE_HASH_LOCK:
		if (data_vio->hash_lock != NULL) {
			launch_data_vio_hash_zone_callback(data_vio,
							   clean_hash_lock);
			return;
		}
		fallthrough;

	case VIO_RELEASE_LOGICAL:
		launch_data_vio_logical_callback(data_vio,
						 release_logical_lock);
		return;

	default:
		finish_cleanup(data_vio);
	}
}

/**
 * complete_data_vio() - Complete the processing of a data_vio.
 * @completion: The completion of the vio to complete.
 */
void complete_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	completion->error_handler = NULL;
	if (completion->result != VDO_SUCCESS)
		update_data_vio_error_stats(data_vio);

	data_vio->last_async_operation = VIO_ASYNC_OP_CLEANUP;
	perform_cleanup_stage(data_vio,
			      (is_read_data_vio(data_vio)
			       ? VIO_RELEASE_LOGICAL
			       : VIO_CLEANUP_START));
}

/**
 * finish_data_vio() - Finish processing a data_vio.
 * @data_vio: The data_vio.
 * @result: The result of processing the data_vio.
 *
 * This function will set any error, and then initiate data_vio clean up.
 */
void finish_data_vio(struct data_vio *data_vio, int result)
{
	struct vdo_completion *completion = data_vio_as_completion(data_vio);

	vdo_set_completion_result(completion, result);
	complete_data_vio(completion);
}

/**
 * get_data_vio_operation_name() - Get the name of the last asynchronous
 *				   operation performed on a data_vio.
 * @data_vio: The data_vio in question.
 *
 * Return: The name of the last operation performed on the data_vio.
 */
const char *get_data_vio_operation_name(struct data_vio *data_vio)
{
	STATIC_ASSERT((MAX_VIO_ASYNC_OPERATION_NUMBER -
		       MIN_VIO_ASYNC_OPERATION_NUMBER) ==
		      ARRAY_SIZE(ASYNC_OPERATION_NAMES));

	return ((data_vio->last_async_operation <
		 MAX_VIO_ASYNC_OPERATION_NUMBER) ?
			ASYNC_OPERATION_NAMES[data_vio->last_async_operation] :
			"unknown async operation");
}

/**
 * set_data_vio_duplicate_location() - Set the location of the duplicate block
 *				       for a data_vio, updating the
 *				       is_duplicate and duplicate fields from
 *				       a zoned_pbn.
 * @data_vio: The data_vio to modify.
 * @source: The location of the duplicate.
 */
void set_data_vio_duplicate_location(struct data_vio *data_vio,
				     const struct zoned_pbn source)
{
	data_vio->is_duplicate = (source.pbn != VDO_ZERO_BLOCK);
	data_vio->duplicate = source;
}

/**
 * clear_data_vio_mapped_location() - Clear a data_vio's mapped block
 *				      location, setting it to be unmapped.
 * @data_vio: The data_vio whose mapped block location is to be reset.
 *
 * This indicates the block map entry for the logical block is either unmapped
 * or corrupted.
 */
void clear_data_vio_mapped_location(struct data_vio *data_vio)
{
	data_vio->mapped = (struct zoned_pbn) {
		.state = VDO_MAPPING_STATE_UNMAPPED,
	};
}

/**
 * set_data_vio_mapped_location() - Set a data_vio's mapped field to the
 *				    physical location recorded in the block
 *				    map for the logical block in the vio.
 * @data_vio: The data_vio whose field is to be set.
 * @pbn: The physical block number to set.
 * @state: The mapping state to set.
 *
 * Return: VDO_SUCCESS or an error code if the mapping is unusable.
 */
int set_data_vio_mapped_location(struct data_vio *data_vio,
				 physical_block_number_t pbn,
				 enum block_mapping_state state)
{
	struct physical_zone *zone;
	int result = vdo_get_physical_zone(vdo_from_data_vio(data_vio),
					   pbn, &zone);
	if (result != VDO_SUCCESS)
		return result;

	data_vio->mapped = (struct zoned_pbn) {
		.pbn = pbn,
		.state = state,
		.zone = zone,
	};
	return VDO_SUCCESS;
}

/**
 * launch_locked_request() - Launch a request which has acquired an LBN lock.
 * @data_vio: The data_vio which has just acquired a lock.
 */
static void launch_locked_request(struct data_vio *data_vio)
{
	data_vio->logical.locked = true;
	if (!is_read_data_vio(data_vio)) {
		struct vdo *vdo = vdo_from_data_vio(data_vio);

		if (vdo_is_read_only(vdo->read_only_notifier)) {
			finish_data_vio(data_vio, VDO_READ_ONLY);
			return;
		}
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
	vdo_find_block_map_slot(data_vio);
}

/**
 * attempt_logical_block_lock() - Attempt to acquire the lock on a logical
 *				  block.
 * @completion: The data_vio for an external data request as a completion.
 *
 * This is the start of the path for all external requests. It is registered
 * in launch_data_vio().
 */
void attempt_logical_block_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct lbn_lock *lock = &data_vio->logical;
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct data_vio *lock_holder;
	int result;

	assert_data_vio_in_logical_zone(data_vio);

	if (data_vio->logical.lbn >= vdo->states.vdo.config.logical_blocks) {
		finish_data_vio(data_vio, VDO_OUT_OF_RANGE);
		return;
	}

	result = int_map_put(lock->zone->lbn_operations,
			     lock->lbn,
			     data_vio,
			     false,
			     (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	if (lock_holder == NULL) {
		/* We got the lock */
		launch_locked_request(data_vio);
		return;
	}

	result = ASSERT(lock_holder->logical.locked,
			"logical block lock held");
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	/*
	 * If the new request is a pure read request (not read-modify-write)
	 * and the lock_holder is writing and has received an allocation
	 * (VDO-2683), service the read request immediately by copying data
	 * from the lock_holder to avoid having to flush the write out of the
	 * packer just to prevent the read from waiting indefinitely. If the
	 * lock_holder does not yet have an allocation, prevent it from
	 * blocking in the packer and wait on it.
	 */
	if (is_read_data_vio(data_vio) &&
	    READ_ONCE(lock_holder->allocation_succeeded)) {
		vdo_bio_copy_data_out(data_vio->user_bio,
				      (lock_holder->data_block +
				       data_vio->offset));
		acknowledge_data_vio(data_vio);
		complete_data_vio(completion);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_LOGICAL_BLOCK_LOCK;
	result = enqueue_data_vio(&lock_holder->logical.waiters,
				  data_vio);
	if (result != VDO_SUCCESS) {
		finish_data_vio(data_vio, result);
		return;
	}

	/*
	 * Prevent writes and read-modify-writes from blocking indefinitely on
	 * lock holders in the packer.
	 */
	if (!is_read_data_vio(lock_holder) &&
	    cancel_vio_compression(lock_holder)) {
		data_vio->compression.lock_holder = lock_holder;
		launch_data_vio_packer_callback(data_vio,
						vdo_remove_lock_holder_from_packer);
	}
}

/**
 * release_lock() - Release an uncontended LBN lock.
 * @data_vio: The data_vio holding the lock.
 */
static void release_lock(struct data_vio *data_vio)
{
	struct lbn_lock *lock = &data_vio->logical;
	struct int_map *lock_map = lock->zone->lbn_operations;
	struct data_vio *lock_holder;

	if (!lock->locked) {
		/*
		 * The lock is not locked, so it had better not be registered
		 * in the lock map.
		 */
		struct data_vio *lock_holder = int_map_get(lock_map, lock->lbn);

		ASSERT_LOG_ONLY((data_vio != lock_holder),
				"no logical block lock held for block %llu",
				(unsigned long long) lock->lbn);
		return;
	}

	/* Remove the lock from the logical block lock map, releasing the lock. */
	lock_holder = int_map_remove(lock_map, lock->lbn);
	ASSERT_LOG_ONLY((data_vio == lock_holder),
			"logical block lock mismatch for block %llu",
			(unsigned long long) lock->lbn);
	lock->locked = false;
}

/**
 * vdo_release_logical_block_lock() - Release the lock on the logical block,
 *				      if any, that a data_vio has acquired.
 * @data_vio: The data_vio releasing its logical block lock.
 */
void vdo_release_logical_block_lock(struct data_vio *data_vio)
{
	struct data_vio *lock_holder, *next_lock_holder;
	struct lbn_lock *lock = &data_vio->logical;
	int result;

	assert_data_vio_in_logical_zone(data_vio);
	if (!has_waiters(&data_vio->logical.waiters)) {
		release_lock(data_vio);
		return;
	}

	ASSERT_LOG_ONLY(lock->locked, "lbn_lock with waiters is not locked");

	/*
	 * Another data_vio is waiting for the lock, so just transfer it in a
	 * single lock map operation
	 */
	next_lock_holder =
		waiter_as_data_vio(dequeue_next_waiter(&lock->waiters));

	/* Transfer the remaining lock waiters to the next lock holder. */
	transfer_all_waiters(&lock->waiters,
			     &next_lock_holder->logical.waiters);

	result = int_map_put(lock->zone->lbn_operations,
			     lock->lbn,
			     next_lock_holder,
			     true,
			     (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		finish_data_vio(next_lock_holder, result);
		return;
	}

	ASSERT_LOG_ONLY((lock_holder == data_vio),
			"logical block lock mismatch for block %llu",
			(unsigned long long) lock->lbn);
	lock->locked = false;

	/*
	 * If there are still waiters, other data_vios must be trying to get
	 * the lock we just transferred. We must ensure that the new lock
	 * holder doesn't block in the packer.
	 */
	if (has_waiters(&next_lock_holder->logical.waiters))
		cancel_vio_compression(next_lock_holder);

	/*
	 * Avoid stack overflow on lock transfer.
	 * XXX: this is only an issue in the 1 thread config.
	 */
	data_vio_as_completion(next_lock_holder)->requeue = true;
	launch_locked_request(next_lock_holder);
}

/**
 * data_vio_allocate_data_block() - Allocate a data block.
 *
 * @data_vio: The data_vio which needs an allocation.
 * @write_lock_type: The type of write lock to obtain on the block.
 * @callback: The callback which will attempt an allocation in the current
 *	      zone and continue if it succeeds.
 * @error_handler: The handler for errors while allocating.
 */
void data_vio_allocate_data_block(struct data_vio *data_vio,
				  enum pbn_lock_type write_lock_type,
				  vdo_action *callback,
				  vdo_action *error_handler)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct allocation *allocation = &data_vio->allocation;
	struct allocation_selector *selector =
		data_vio->logical.zone->selector;

	ASSERT_LOG_ONLY((allocation->pbn == VDO_ZERO_BLOCK),
			"data_vio does not have an allocation");
	allocation->write_lock_type = write_lock_type;
	allocation->first_allocation_zone =
		vdo_get_next_allocation_zone(selector);
	allocation->zone =
		&vdo->physical_zones->zones[allocation->first_allocation_zone];

	data_vio_as_completion(data_vio)->error_handler = error_handler;
	launch_data_vio_allocated_zone_callback(data_vio, callback);
}

void release_data_vio_allocation_lock(struct data_vio *data_vio, bool reset)
{
	struct allocation *allocation = &data_vio->allocation;
	physical_block_number_t locked_pbn = allocation->pbn;

	assert_data_vio_in_allocated_zone(data_vio);

	if (reset ||
	    vdo_pbn_lock_has_provisional_reference(allocation->lock))
		allocation->pbn = VDO_ZERO_BLOCK;

	vdo_release_physical_zone_pbn_lock(allocation->zone,
					   locked_pbn,
					   UDS_FORGET(allocation->lock));
}

void acknowledge_data_vio(struct data_vio *data_vio)
{
	struct vdo *vdo = vdo_from_data_vio(data_vio);
	struct bio *bio = data_vio->user_bio;
	int error = vdo_map_to_system_error(data_vio_as_completion(data_vio)->result);
#ifdef VDO_INTERNAL
	uint64_t latency_jiffies;
	unsigned int ack_msecs;
	struct vdo_histograms *histograms = &vdo->histograms;
#endif /* VDO_INTERNAL */

	if (bio == NULL)
		return;

	ASSERT_LOG_ONLY((data_vio->remaining_discard <=
			 (uint32_t) (VDO_BLOCK_SIZE - data_vio->offset)),
			"data_vio to acknowledge is not an incomplete discard");

	data_vio->user_bio = NULL;
	vdo_count_bios(&vdo->stats.bios_acknowledged, bio);
	if (data_vio->is_partial)
		vdo_count_bios(&vdo->stats.bios_acknowledged_partial, bio);

#ifdef VDO_INTERNAL
	latency_jiffies = jiffies - data_vio->arrival_jiffies;
	ack_msecs = jiffies_to_msecs(latency_jiffies);
	if (bio_data_dir(bio) != WRITE)
		enter_histogram_sample(histograms->read_ack_histogram,
				       latency_jiffies);
	else if (bio_op(bio) == REQ_OP_DISCARD)
		enter_histogram_sample(histograms->discard_ack_histogram,
				       latency_jiffies);
	else
		enter_histogram_sample(histograms->write_ack_histogram,
				       latency_jiffies);

	if (ack_msecs > 30000) {
		static DEFINE_RATELIMIT_STATE(latency_limiter,
					      DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);

		if (__ratelimit(&latency_limiter)) {
			uds_log_info("Acknowledgement Latency Violation: %u msecs, error %d",
				     ack_msecs, -error);
			dump_data_vio(data_vio);
		}
	}

#endif

	vdo_complete_bio(bio, error);
}

/**
 * compress_data_vio() - A function to compress the data in a data_vio.
 * @data_vio: The data_vio to compress.
 */
void compress_data_vio(struct data_vio *data_vio)
{
	int size;
	char *context = get_work_queue_private_data();

	/*
	 * By putting the compressed data at the start of the compressed
	 * block data field, we won't need to copy it if this data_vio
	 * becomes a compressed write agent.
	 */
	size = LZ4_compress_default(data_vio->data_block,
				    data_vio->compression.block->data,
				    VDO_BLOCK_SIZE,
				    VDO_MAX_COMPRESSED_FRAGMENT_SIZE,
				    context);
	if (size > 0)
		data_vio->compression.size = size;
	else
		/*
		 * Use block size plus one as an indicator for uncompressible
		 * data.
		 */
		data_vio->compression.size = VDO_BLOCK_SIZE + 1;
}

/**
 * uncompress_data_vio() - A function to uncompress the data a data_vio has
 *			   just read.
 * @data_vio: The data_vio to uncompress.
 * @mapping_state: The mapping state indicating which fragment to decompress.
 * @buffer: The buffer to receive the uncompressed data.
 */
int uncompress_data_vio(struct data_vio *data_vio,
			enum block_mapping_state mapping_state,
			char *buffer)
{
	int size;
	uint16_t fragment_offset, fragment_size;
	struct compressed_block *block = data_vio->compression.block;
	int result = vdo_get_compressed_block_fragment(mapping_state,
						       block,
						       &fragment_offset,
						       &fragment_size);

	if (result != VDO_SUCCESS) {
		uds_log_debug("%s: compressed fragment error %d",
			      __func__,
			      result);
		return result;
	}

	size = LZ4_decompress_safe((block->data + fragment_offset),
				   buffer,
				   fragment_size,
				   VDO_BLOCK_SIZE);
	if (size != VDO_BLOCK_SIZE) {
		uds_log_debug("%s: lz4 error", __func__);
		return VDO_INVALID_FRAGMENT;
	}

	return VDO_SUCCESS;
}

/* Return true if a data block contains all zeros. */
bool is_zero_block(char *block)
{
	int i;

#ifdef INTERNAL
	STATIC_ASSERT(VDO_BLOCK_SIZE % sizeof(uint64_t) == 0);
	ASSERT_LOG_ONLY((uintptr_t) block % sizeof(uint64_t) == 0,
			"Data blocks are expected to be aligned");
#endif	/* INTERNAL */

	for (i = 0; i < VDO_BLOCK_SIZE; i += sizeof(uint64_t))
		if (*((uint64_t *) &block[i]))
			return false;
	return true;
}

/**
 * modify_for_partial_write() - Do the modify-write part of a
 *                              read-modify-write cycle.
 * @completion: The data_vio which has just finished its read.
 *
 * This callback is registered in read_block().
 */
static void modify_for_partial_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct bio *bio = data_vio->user_bio;

	assert_data_vio_on_cpu_thread(data_vio);

	if (bio_op(bio) == REQ_OP_DISCARD) {
		memset(data_vio->data_block + data_vio->offset,
		       '\0',
		       min_t(uint32_t,
			     data_vio->remaining_discard,
			     VDO_BLOCK_SIZE - data_vio->offset));
	} else {
		vdo_bio_copy_data_in(bio,
				     data_vio->data_block + data_vio->offset);
	}

	data_vio->is_zero_block = is_zero_block(data_vio->data_block);
	data_vio->io_operation =
		(DATA_VIO_WRITE |
		 (data_vio->io_operation & ~DATA_VIO_READ_WRITE_MASK));
	completion->error_handler = NULL;
	launch_data_vio_logical_callback(data_vio,
					 continue_data_vio_with_block_map_slot);
}

static void complete_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	bool compressed = vdo_is_state_compressed(data_vio->mapped.state);

	assert_data_vio_on_cpu_thread(data_vio);

	if (compressed) {
		int result = uncompress_data_vio(data_vio,
						 data_vio->mapped.state,
						 data_vio->data_block);

		if (result != VDO_SUCCESS) {
			finish_data_vio(data_vio, result);
			return;
		}
	}

	if (is_read_modify_write_data_vio(data_vio)) {
		modify_for_partial_write(completion);
		return;
	}

	if (compressed || data_vio->is_partial) {
		vdo_bio_copy_data_out(data_vio->user_bio,
				      data_vio->data_block + data_vio->offset);
	}

	acknowledge_data_vio(data_vio);
	complete_data_vio(completion);
}

static void read_endio(struct bio *bio)
{
	struct data_vio *data_vio = vio_as_data_vio(bio->bi_private);
	int result = blk_status_to_errno(bio->bi_status);

	vdo_count_completed_bios(bio);
	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	launch_data_vio_cpu_callback(data_vio,
				     complete_read,
				     CPU_Q_COMPLETE_READ_PRIORITY);
}

static void complete_zero_read(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);

	if (data_vio->is_partial) {
		memset(data_vio->data_block, 0, VDO_BLOCK_SIZE);
		if (!is_read_data_vio(data_vio)) {
			modify_for_partial_write(completion);
			return;
		}
	} else {
		zero_fill_bio(data_vio->user_bio);
	}

	complete_read(completion);
}

/**
 * read_block() - Read a block asynchronously.
 * @completion: The data_vio to read.
 *
 * This is the callback registered in read_block_mapping().
 */
static void read_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vio *vio = as_vio(completion);
	int result = VDO_SUCCESS;

	if (completion->result != VDO_SUCCESS) {
		complete_data_vio(completion);
		return;
	}

	completion->error_handler = complete_data_vio;

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK) {
		launch_data_vio_cpu_callback(data_vio,
					     complete_zero_read,
					     CPU_Q_COMPLETE_VIO_PRIORITY);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_READ_DATA_VIO;
	completion->error_handler = complete_data_vio;
	if (vdo_is_state_compressed(data_vio->mapped.state)) {
		result = prepare_data_vio_for_io(data_vio,
						 (char *) data_vio->compression.block,
						 read_endio,
						 REQ_OP_READ,
						 data_vio->mapped.pbn);
	} else {
		int opf = ((data_vio->user_bio->bi_opf & PASSTHROUGH_FLAGS) |
			   REQ_OP_READ);

		if (is_read_modify_write_data_vio(data_vio) ||
		    (data_vio->is_partial)) {
			result = prepare_data_vio_for_io(data_vio,
							 data_vio->data_block,
							 read_endio,
							 opf,
							 data_vio->mapped.pbn);
		} else {
			/*
			 * A full 4k read. Use the incoming bio to avoid having
			 * to copy the data
			 */
#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
#endif

#if USE_ALTERNATE
			bio_reset(vio->bio);
			__bio_clone_fast(vio->bio, data_vio->user_bio);
#else
			bio_reset(vio->bio, vio->bio->bi_bdev, opf);
			bio_init_clone(data_vio->user_bio->bi_bdev,
				       vio->bio,
				       data_vio->user_bio,
				       GFP_KERNEL);
#endif

			/* Copy over the original bio iovec and opflags. */
			vdo_set_bio_properties(vio->bio,
					       vio,
					       read_endio,
					       opf,
					       data_vio->mapped.pbn);
		}
	}

	if (result != VDO_SUCCESS) {
		continue_data_vio_with_error(data_vio, result);
		return;
	}

	submit_data_vio_io(data_vio);
}

/**
 * finish_write_data_vio_with_error() - Return a data_vio that encountered an
 *					error to its hash lock so it can
 *					update the hash lock state
 *					accordingly.
 * @completion: The completion of the data_vio to return to its hash lock.
 *
 * This continuation is registered in abort_on_error(), and must be called in
 * the hash zone of the data_vio.
 */
static void finish_write_data_vio_with_error(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	vdo_continue_hash_lock_on_error(data_vio);
}

/**
 * abort_on_error() - Check whether a result is an error, and if so abort the
 *		      data_vio associated with the error.
 * @result: The result to check.
 * @data_vio: The data_vio.
 * @action: The conditions under which the VDO should be put into read-only
 *	    mode if the result is an error.
 *
 * Return: true if the result is an error.
 */
static bool abort_on_error(int result,
			   struct data_vio *data_vio,
			   enum read_only_action action)
{
	if (result == VDO_SUCCESS)
		return false;

	if ((result == VDO_READ_ONLY) || (action == READ_ONLY)) {
		struct read_only_notifier *notifier =
			vdo_from_data_vio(data_vio)->read_only_notifier;
		if (!vdo_is_read_only(notifier)) {
			if (result != VDO_READ_ONLY)
				uds_log_error_strerror(result,
						       "Preparing to enter read-only mode: data_vio for LBN %llu (becoming mapped to %llu, previously mapped to %llu, allocated %llu) is completing with a fatal error after operation %s",
						       (unsigned long long) data_vio->logical.lbn,
						       (unsigned long long) data_vio->new_mapped.pbn,
						       (unsigned long long) data_vio->mapped.pbn,
						       (unsigned long long) get_data_vio_allocation(data_vio),
						       get_data_vio_operation_name(data_vio));

			vdo_enter_read_only_mode(notifier, result);
		}
	}

	if (data_vio->hash_lock != NULL)
		launch_data_vio_hash_zone_callback(data_vio,
						   finish_write_data_vio_with_error);
	else
		finish_data_vio(data_vio, result);
	return true;
}

/**
 * finish_write_data_vio() - Return a finished data_vio to its hash lock.
 * @completion: The completion of the data_vio to return to its hash lock.
 *
 * Returns a data_vio that finished writing, compressing, or deduplicating to
 * its hash lock so it can share the result with any data_vios waiting in the
 * hash lock, or update UDS, or simply release its share of the lock. This
 * continuation is registered in update_block_map_for_write(),
 * update_block_map_for_dedupe(), and abort_deduplication(), and must be
 * called in the hash zone of the data_vio.
 */
static void finish_write_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_hash_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;
	vdo_continue_hash_lock(data_vio);
}

static void write_block(struct data_vio *data_vio);

/**
 * abort_deduplication() - Abort the data optimization process.
 * @data_vio: The data_vio which does not deduplicate or compress.
 */
static void abort_deduplication(struct data_vio *data_vio)
{
	if (!data_vio_has_allocation(data_vio)) {
		/*
		 * There was no space to write this block and we failed to
		 * deduplicate or compress it.
		 */
		finish_data_vio(data_vio, VDO_NO_SPACE);
		return;
	}

	/*
	 * We failed to deduplicate or compress so now we need to actually
	 * write the data.
	 */
	write_block(data_vio);
}

/**
 * update_block_map_for_dedupe() - Update the block map now that we've added
 * an entry in the recovery journal for a block we have just shared.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered in decrement_for_dedupe().
 */
static void update_block_map_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->hash_lock != NULL)
		set_data_vio_hash_zone_callback(data_vio,
						finish_write_data_vio);
	else
		completion->callback = complete_data_vio;
	data_vio->last_async_operation = VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_DEDUPE;
	vdo_put_mapped_block(data_vio);
}

/**
 * journal_increment() - Make a recovery journal increment.
 * @data_vio: The data_vio.
 * @lock: The pbn_lock on the block being incremented.
 */
static void journal_increment(struct data_vio *data_vio, struct pbn_lock *lock)
{
	vdo_set_up_reference_operation_with_lock(VDO_JOURNAL_DATA_INCREMENT,
						 data_vio->new_mapped.pbn,
						 data_vio->new_mapped.state,
						 lock,
						 &data_vio->operation);
	vdo_add_recovery_journal_entry(vdo_from_data_vio(data_vio)->recovery_journal,
				       data_vio);
}

/**
 * journal_decrement() - Make a recovery journal decrement entry.
 * @data_vio: The data_vio.
 */
static void journal_decrement(struct data_vio *data_vio)
{
	vdo_set_up_reference_operation_with_zone(VDO_JOURNAL_DATA_DECREMENT,
						 data_vio->mapped.pbn,
						 data_vio->mapped.state,
						 data_vio->mapped.zone,
						 &data_vio->operation);
	vdo_add_recovery_journal_entry(vdo_from_data_vio(data_vio)->recovery_journal,
				       data_vio);
}

/**
 * update_reference_count() - Make a reference count change.
 * @data_vio: The data_vio.
 */
static void update_reference_count(struct data_vio *data_vio)
{
	struct slab_depot *depot = vdo_from_data_vio(data_vio)->depot;
	physical_block_number_t pbn = data_vio->operation.pbn;
	int result =
		ASSERT(vdo_is_physical_data_block(depot, pbn),
		       "Adding slab journal entry for impossible PBN %llu for LBN %llu",
		       (unsigned long long) pbn,
		       (unsigned long long) data_vio->logical.lbn);
	if (abort_on_error(result, data_vio, READ_ONLY))
		return;

	vdo_add_slab_journal_entry(vdo_get_slab_journal(depot, pbn), data_vio);
}

/**
 * decrement_for_dedupe() - Do the decref after a successful dedupe or
 *			    compression.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by journal_unmapping_for_dedupe().
 */
static void decrement_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->allocation.pbn == data_vio->mapped.pbn)
		/*
		 * If we are about to release the reference on the allocated
		 * block, we must release the PBN lock on it first so that the
		 * allocator will not allocate a write-locked block.
		 *
		 * FIXME: now that we don't have sync mode, can this ever
		 *	  happen?
		 */
		release_data_vio_allocation_lock(data_vio, false);

	set_data_vio_logical_callback(data_vio, update_block_map_for_dedupe);
	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_DEDUPE;
	update_reference_count(data_vio);
}

/**
 * journal_unmapping_for_dedupe() - Write the appropriate journal entry for
 *				    removing the mapping of logical to mapped,
 *				    for dedupe or compression.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered in read_old_block_mapping_for_dedupe().
 */
static void journal_unmapping_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK)
		set_data_vio_logical_callback(data_vio,
					      update_block_map_for_dedupe);
	else
		set_data_vio_mapped_zone_callback(data_vio,
						  decrement_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_DEDUPE;
	journal_decrement(data_vio);
}

/**
 * read_old_block_mapping_for_dedupe() - Get the prevoius PBN/LBN mapping.
 * @completion: The completion of the write in progress.
 *
 * Gets the previous PBN mapped to this LBN from the block map, so as to make
 * an appropriate journal entry referencing the removal of this LBN->PBN
 * mapping, for dedupe or compression. This callback is registered in
 * increment_for_dedupe().
 */
static void read_old_block_mapping_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_DEDUPE;
	set_data_vio_journal_callback(data_vio, journal_unmapping_for_dedupe);
	vdo_get_mapped_block(data_vio);
}

/**
 * increment_for_compression() - Do the incref after compression.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by
 * add_recovery_journal_entry_for_compression().
 */
static void increment_for_compression(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	ASSERT_LOG_ONLY(vdo_is_state_compressed(data_vio->new_mapped.state),
			"Impossible attempt to update reference counts for a block which was not compressed (logical block %llu)",
			(unsigned long long) data_vio->logical.lbn);

	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_dedupe);
	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_COMPRESSION;
	update_reference_count(data_vio);
}

/**
 * add_recovery_journal_entry_for_compression() - Add a recovery journal entry
 *						  for the increment resulting
 *						  from compression.
 * @completion: The data_vio which has been compressed.
 *
 * This callback is registered in continue_write_after_compression().
 */
static void
add_recovery_journal_entry_for_compression(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	set_data_vio_new_mapped_zone_callback(data_vio,
					      increment_for_compression);
	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_COMPRESSION;
	journal_increment(data_vio, vdo_get_duplicate_lock(data_vio));
}

/**
 * continue_write_after_compression() - Continue a write after the data_vio
 *					has been released from the packer.
 * @data_vio: The data_vio which has returned from the packer.
 *
 * The write may or may not have been written as part of a compressed write.
 */
void continue_write_after_compression(struct data_vio *data_vio)
{
	if (!vdo_is_state_compressed(data_vio->new_mapped.state)) {
		abort_deduplication(data_vio);
		return;
	}

	launch_data_vio_journal_callback(data_vio,
					 add_recovery_journal_entry_for_compression);
}

/**
 * pack_compressed_data() - Attempt to pack the compressed data_vio into a
 *			    block.
 * @completion: The completion of a compressed data_vio.
 *
 * This is the callback registered in launch_compress_data_vio().
 */
static void pack_compressed_data(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_packer_zone(data_vio);

	/*
	 * XXX this is a callback, so there should probably be an error check
	 * here even if we think compression can't currently return one.
	 */

	if (!may_pack_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ATTEMPT_PACKING;
	vdo_attempt_packing(data_vio);
}

/**
 * compress_data_vio_callback() - Do the actual work of compressing the data
 *				  on a CPU queue.
 * @completion: The completion of the write in progress.
 *
 * This callback is registered in launch_compress_data_vio().
 */
static void compress_data_vio_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);
	compress_data_vio(data_vio);
	launch_data_vio_packer_callback(data_vio,
					pack_compressed_data);
}

/**
 * launch_compress_data_vio() - Continue a write by attempting to compress the
 *				data.
 * @data_vio: The data_vio to be compressed.
 *
 * This is a re-entry point to vio_write used by hash locks.
 */
void launch_compress_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(!data_vio->is_duplicate,
			"compressing a non-duplicate block");
	if (!may_compress_data_vio(data_vio)) {
		abort_deduplication(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_COMPRESS_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio,
				     compress_data_vio_callback,
				     CPU_Q_COMPRESS_BLOCK_PRIORITY);
}

/**
 * increment_for_dedupe() - Do the incref after deduplication.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by add_recovery_journal_entry_for_dedupe().
 */
static void increment_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_new_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_DEDUPE;
	update_reference_count(data_vio);
}

/**
 * add_recovery_journal_entry_for_dedupe() - Add a recovery journal entry for
 *					     the increment resulting from
 *					     deduplication.
 * @completion: The data_vio which has been deduplicated.
 *
 * This callback is registered in launch_deduplicate_data_vio().
 */
static void
add_recovery_journal_entry_for_dedupe(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	set_data_vio_new_mapped_zone_callback(data_vio, increment_for_dedupe);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_DEDUPE;
	journal_increment(data_vio, vdo_get_duplicate_lock(data_vio));
}

/**
 * launch_deduplicate_data_vio() - Continue a write by deduplicating a write
 *				   data_vio against a verified existing block
 *				   containing the data.
 * @data_vio: The data_vio to be deduplicated.
 *
 * This is a re-entry point to vio_write used by hash locks.
 */
void launch_deduplicate_data_vio(struct data_vio *data_vio)
{
	ASSERT_LOG_ONLY(data_vio->is_duplicate,
			"data_vio must have a duplicate location");

	data_vio->new_mapped = data_vio->duplicate;
	launch_data_vio_journal_callback(data_vio,
					 add_recovery_journal_entry_for_dedupe);
}

/**
 * lock_hash_in_zone() - Route the data_vio to the hash_zone responsible for
 *			 the record name to acquire a hash lock on that name,
 *			 or join with a existing hash lock managing concurrent
 *			 dedupe for that name.
 * @completion: The data_vio to lock.
 *
 * This is the callback registered in hash_data_vio().
 */
static void lock_hash_in_zone(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	int result;

	assert_data_vio_in_hash_zone(data_vio);
	/* Shouldn't have had any errors since all we did was switch threads. */
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	result = vdo_acquire_hash_lock(data_vio);
	if (abort_on_error(result, data_vio, READ_ONLY))
		return;

	if (data_vio->hash_lock == NULL) {
		/*
		 * It's extremely unlikely, but in the case of a hash
		 * collision, the data_vio will not obtain a reference to the
		 * lock and cannot deduplicate.
		 */
		launch_compress_data_vio(data_vio);
		return;
	}

	vdo_enter_hash_lock(data_vio);
}

/**
 * hash_data_vio() - Hash the data in a data_vio and set the hash zone (which
 *		     also flags the record name as set).
 * @completion: The data_vio to hash.

 * This callback is registered in prepare_for_dedupe().
 */
static void hash_data_vio(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_on_cpu_thread(data_vio);
	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero blocks should not be hashed");

	murmurhash3_128(data_vio->data_block,
			VDO_BLOCK_SIZE,
			0x62ea60be,
			&data_vio->record_name);

	data_vio->hash_zone =
		vdo_select_hash_zone(vdo_from_data_vio(data_vio)->hash_zones,
				     &data_vio->record_name);
	data_vio->last_async_operation = VIO_ASYNC_OP_ACQUIRE_VDO_HASH_LOCK;
	launch_data_vio_hash_zone_callback(data_vio,
					   lock_hash_in_zone);
}

/**
 * prepare_for_dedupe() - Prepare for the dedupe path after attempting to get
 *			  an allocation.
 * @data_vio: The data_vio to deduplicate.
 */
static void prepare_for_dedupe(struct data_vio *data_vio)
{
	/* We don't care what thread we are on */
	if (abort_on_error(data_vio_as_completion(data_vio)->result,
			   data_vio,
			   READ_ONLY))
		return;

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"must not prepare to dedupe zero blocks");

	/*
	 * Before we can dedupe, we need to know the record name, so the first
	 * step is to hash the block data.
	 */
	data_vio->last_async_operation = VIO_ASYNC_OP_HASH_DATA_VIO;
	launch_data_vio_cpu_callback(data_vio,
				     hash_data_vio,
				     CPU_Q_HASH_BLOCK_PRIORITY);
}

/**
 * update_block_map_for_write() - Update the block map after a data write (or
 *				  directly for a VDO_ZERO_BLOCK write or
 *				  trim).
 * @completion: The completion of the write in progress.
 *
 * This callback is registered in decrement_for_write() and
 * journal_unmapping_for_write().
 */
static void update_block_map_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->hash_lock != NULL)
		/*
		 * The write is finished, but must return to the hash lock to
		 * allow other data VIOs with the same data to dedupe against
		 * the write.
		 */
		set_data_vio_hash_zone_callback(data_vio, finish_write_data_vio);
	else
		completion->callback = complete_data_vio;

	data_vio->last_async_operation = VIO_ASYNC_OP_PUT_MAPPED_BLOCK_FOR_WRITE;
	vdo_put_mapped_block(data_vio);
}

/**
 * decrement_for_write() - Do the decref after a successful block write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback by journal_unmapping_for_write() if the old mapping
 * was not the zero block.
 */
static void decrement_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_mapped_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_DECREMENT_FOR_WRITE;
	set_data_vio_logical_callback(data_vio, update_block_map_for_write);
	update_reference_count(data_vio);
}

/**
 * journal_unmapping_for_write() - Write the appropriate journal entry for
 *				   unmapping logical to mapped for a write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered in read_old_block_mapping_for_write().
 */
static void journal_unmapping_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->mapped.pbn == VDO_ZERO_BLOCK)
		set_data_vio_logical_callback(data_vio,
					      update_block_map_for_write);
	else
		set_data_vio_mapped_zone_callback(data_vio,
						  decrement_for_write);
	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_UNMAPPING_FOR_WRITE;
	journal_decrement(data_vio);
}

/**
 * read_old_block_mapping_for_write() - Get the previous PBN mapped to this
 *					LBN from the block map for a write, so
 *					as to make an appropriate journal
 *					entry referencing the removal of this
 *					LBN->PBN mapping.
 * @completion: The completion of the write in progress.
 *
 * This callback is registered in finish_block_write().
 */
static void read_old_block_mapping_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	set_data_vio_journal_callback(data_vio, journal_unmapping_for_write);
	data_vio->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_WRITE;
	vdo_get_mapped_block(data_vio);
}

/**
 * increment_for_write() - Do the incref after a successful block write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by finish_block_write().
 */
static void increment_for_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	/*
	 * Now that the data has been written, it's safe to deduplicate against
	 * the block. Downgrade the allocation lock to a read lock so it can be
	 * used later by the hash lock.
	 */
	vdo_downgrade_pbn_write_lock(data_vio->allocation.lock, false);

	data_vio->last_async_operation =
		VIO_ASYNC_OP_JOURNAL_INCREMENT_FOR_WRITE;
	set_data_vio_logical_callback(data_vio,
				      read_old_block_mapping_for_write);
	update_reference_count(data_vio);
}

/**
 * finish_block_write() - Add an entry in the recovery journal after a
 *			  successful block write.
 * @completion: The completion of the write in progress.
 *
 * This is the callback registered by write_block(). It is also registered in
 * allocate_block_for_write().
 */
static void finish_block_write(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);
	if (abort_on_error(completion->result, data_vio, READ_ONLY))
		return;

	if (data_vio->new_mapped.pbn == VDO_ZERO_BLOCK)
		set_data_vio_logical_callback(data_vio,
					      read_old_block_mapping_for_write);
	else
		set_data_vio_allocated_zone_callback(data_vio,
						     increment_for_write);

	data_vio->last_async_operation = VIO_ASYNC_OP_JOURNAL_MAPPING_FOR_WRITE;
	journal_increment(data_vio, data_vio->allocation.lock);
}

/**
 * write_bio_finished() - This is the bio_end_io functon registered in
 *			  write_block() to be called when a data_vio's write
 *			  to the underlying storage has completed.
 * @bio: The bio which has just completed.
 */
static void write_bio_finished(struct bio *bio)
{
	struct data_vio *data_vio =
		vio_as_data_vio((struct vio *) bio->bi_private);

	vdo_count_completed_bios(bio);
	vdo_set_completion_result(data_vio_as_completion(data_vio),
				  vdo_get_bio_result(bio));
	launch_data_vio_journal_callback(data_vio,
					 finish_block_write);
}

/**
 * write_block() - Write data to the underlying storage.
 * @data_vio: The data_vio to write.
 */
static void write_block(struct data_vio *data_vio)
{
	int result;

	/* Write the data from the data block buffer. */
	result = prepare_data_vio_for_io(data_vio,
					 data_vio->data_block,
					 write_bio_finished,
					 REQ_OP_WRITE,
					 data_vio->allocation.pbn);
	if (abort_on_error(result, data_vio, READ_ONLY))
		return;

	data_vio->last_async_operation = VIO_ASYNC_OP_WRITE_DATA_VIO;
	submit_data_vio_io(data_vio);
}

/**
 * acknowledge_write_callback() - Acknowledge a write to the requestor.
 * @completion: The data_vio being acknowledged.
 *
 * This callback is registered in allocate_block() and
 * continue_write_with_block_map_slot().
 */
static void acknowledge_write_callback(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct vdo *vdo = completion->vdo;

	ASSERT_LOG_ONLY((!vdo_uses_bio_ack_queue(vdo) ||
			 (vdo_get_callback_thread_id() ==
			     vdo->thread_config->bio_ack_thread)),
			"%s() called on bio ack queue",
			__func__);
	ASSERT_LOG_ONLY(data_vio_has_flush_generation_lock(data_vio),
			"write VIO to be acknowledged has a flush generation lock");
	acknowledge_data_vio(data_vio);
	if (data_vio->new_mapped.pbn == VDO_ZERO_BLOCK) {
		/* This is a zero write or discard */
		launch_data_vio_journal_callback(data_vio, finish_block_write);
		return;
	}

	prepare_for_dedupe(data_vio);
}

/**
 * allocate_block() - Attempt to allocate a block in the current allocation
 *		      zone.
 * @completion: The data_vio needing an allocation.
 *
 * This callback is registered in continue_write_with_block_map_slot().
 */
static void allocate_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);

	if (!vdo_allocate_block_in_zone(data_vio))
		return;

	completion->error_handler = NULL;
	WRITE_ONCE(data_vio->allocation_succeeded, true);
	data_vio->new_mapped = (struct zoned_pbn) {
		.zone = data_vio->allocation.zone,
		.pbn = data_vio->allocation.pbn,
		.state = VDO_MAPPING_STATE_UNCOMPRESSED,
	};

	if (data_vio_requires_fua(data_vio)) {
		prepare_for_dedupe(data_vio);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ACKNOWLEDGE_WRITE;
	launch_data_vio_on_bio_ack_queue(data_vio, acknowledge_write_callback);
}

/**
 * handle_allocation_error() - Handle an error attempting to allocate a block.
 * @completion: The data_vio needing an allocation.
 *
 * This error handler is registered in continue_write_with_block_map_slot().
 */
static void handle_allocation_error(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	completion->error_handler = NULL;
	if (completion->result == VDO_NO_SPACE) {
		/* We failed to get an allocation, but we can try to dedupe. */
		vdo_reset_completion(completion);
		prepare_for_dedupe(data_vio);
		return;
	}

	/*
	 * There was an actual error (not just that we didn't get an
	 * allocation.
	 */
	finish_data_vio(data_vio, completion->result);
}

/**
 * continue_read_with_block_map_slot() - Read the data_vio's mapping from the
 *                                       block map.
 * @completion: The data_vio to be read.
 *
 * This callback is registered in launch_read_data_vio().
 */
void continue_data_vio_with_block_map_slot(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_logical_zone(data_vio);
	if (!is_write_data_vio(data_vio)) {
		set_data_vio_logical_callback(data_vio, read_block);
		data_vio->last_async_operation =
			VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ;
		vdo_get_mapped_block(data_vio);
		return;
	}

	vdo_acquire_flush_generation_lock(data_vio);

	if (data_vio->tree_lock.tree_slots[0].block_map_slot.pbn ==
	    VDO_ZERO_BLOCK) {
		int result =
			ASSERT(is_trim_data_vio(data_vio),
			       "data_vio with no block map page is a trim");
		if (abort_on_error(result, data_vio, READ_ONLY))
			return;

		/*
		 * This is a trim for a block on a block map page which has not
		 * been allocated, so there's nothing more we need to do.
		 */
		finish_data_vio(data_vio, VDO_SUCCESS);
		return;
	}

	if (!data_vio->is_zero_block && !is_trim_data_vio(data_vio)) {
		data_vio_allocate_data_block(data_vio,
					     VIO_WRITE_LOCK,
					     allocate_block,
					     handle_allocation_error);
		return;
	}


	/*
	 * We don't need to write any data, so skip allocation and just
	 * update the block map and reference counts (via the journal).
	 */
	data_vio->new_mapped.pbn = VDO_ZERO_BLOCK;
	if (data_vio->remaining_discard > VDO_BLOCK_SIZE) {
		/*
		 * This is not the final block of a discard so we can't
		 * acknowledge it yet.
		 */
		launch_data_vio_journal_callback(data_vio, finish_block_write);
		return;
	}

	data_vio->last_async_operation = VIO_ASYNC_OP_ACKNOWLEDGE_WRITE;
	launch_data_vio_on_bio_ack_queue(data_vio, acknowledge_write_callback);
}
