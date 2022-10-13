// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vio.h"

#include <linux/kernel.h>
#ifdef __KERNEL__
#include <linux/ratelimit.h>
#endif

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "bio.h"
#include "io-submitter.h"
#include "vdo.h"

/*
 * A vio_pool is a collection of preallocated vios.
 */
struct vio_pool {
	/** The number of objects managed by the pool */
	size_t size;
	/** The list of objects which are available */
	struct list_head available;
	/** The queue of requestors waiting for objects from the pool */
	struct wait_queue waiting;
	/** The number of objects currently in use */
	size_t busy_count;
	/** The list of objects which are in use */
	struct list_head busy;
	/** The ID of the thread on which this pool may be used */
	thread_id_t thread_id;
	/** The buffer backing the pool's vios */
	char *buffer;
	/** The pool entries */
	struct vio_pool_entry entries[];
};

/**
 * create_multi_block_metadata_vio() - Create a vio.
 * @vdo: The vdo on which the vio will operate.
 * @vio_type: The type of vio to create.
 * @priority: The relative priority to assign to the vio.
 * @parent: The parent of the vio.
 * @block_count: The size of the vio in blocks.
 * @data: The buffer.
 * @vio_ptr: A pointer to hold the new vio.
 *
 * Return: VDO_SUCCESS or an error.
 */
int create_multi_block_metadata_vio(struct vdo *vdo,
				    enum vio_type vio_type,
				    enum vio_priority priority,
				    void *parent,
				    unsigned int block_count,
				    char *data,
				    struct vio **vio_ptr)
{
	struct vio *vio;
	struct bio *bio;
	int result;

	/*
	 * If struct vio grows past 256 bytes, we'll lose benefits of
	 * VDOSTORY-176.
	 */
	STATIC_ASSERT(sizeof(struct vio) <= 256);

	result = ASSERT(block_count <= MAX_BLOCKS_PER_VIO,
			"block count %u does not exceed maximum %u",
			block_count,
			MAX_BLOCKS_PER_VIO);
	if (result != VDO_SUCCESS)
		return result;

	result = ASSERT(((vio_type != VIO_TYPE_UNINITIALIZED) &&
			 (vio_type != VIO_TYPE_DATA)),
			"%d is a metadata type",
			vio_type);
	if (result != VDO_SUCCESS)
		return result;

	/*
	 * Metadata vios should use direct allocation and not use the buffer
	 * pool, which is reserved for submissions from the linux block layer.
	 */
	result = UDS_ALLOCATE(1, struct vio, __func__, &vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata vio allocation failure %d", result);
		return result;
	}

	result = vdo_create_multi_block_bio(block_count, &bio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(vio);
		return result;
	}

	initialize_vio(vio,
		       bio,
		       block_count,
		       vio_type,
		       priority,
		       vdo);
	vio->completion.parent = parent;
	vio->data = data;
	*vio_ptr  = vio;
	return VDO_SUCCESS;
}

/**
 * free_vio() - Destroy a vio.
 * @vio: The vio to destroy.
 */
void free_vio(struct vio *vio)
{
	if (vio == NULL)
		return;

	BUG_ON(is_data_vio(vio));
	vdo_free_bio(UDS_FORGET(vio->bio));
	UDS_FREE(vio);
}

/**
 * update_vio_error_stats() - Update per-vio error stats and log the
 *                            error.
 * @vio: The vio which got an error.
 * @format: The format of the message to log (a printf style format).
 */
void update_vio_error_stats(struct vio *vio, const char *format, ...)
{
#ifdef __KERNEL__
	static DEFINE_RATELIMIT_STATE(error_limiter,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
#endif

	va_list args;
	int priority;
	struct vdo_completion *completion = vio_as_completion(vio);

	switch (completion->result) {
	case VDO_READ_ONLY:
		atomic64_inc(&completion->vdo->stats.read_only_error_count);
		return;

	case VDO_NO_SPACE:
		atomic64_inc(&completion->vdo->stats.no_space_error_count);
		priority = UDS_LOG_DEBUG;
		break;

	default:
		priority = UDS_LOG_ERR;
	}

#ifdef __KERNEL__
	if (!__ratelimit(&error_limiter))
		return;
#endif

	va_start(args, format);
	uds_vlog_strerror(priority,
			  completion->result,
			  UDS_LOGGING_MODULE_NAME,
			  format,
			  args);
	va_end(args);
}

void record_metadata_io_error(struct vio *vio)
{
	const char *description;
	physical_block_number_t pbn = pbn_from_vio_bio(vio->bio);

	if (bio_op(vio->bio) == REQ_OP_READ)
		description = "read";
	else if ((vio->bio->bi_opf & REQ_PREFLUSH) == REQ_PREFLUSH)
		description = (((vio->bio->bi_opf & REQ_FUA) == REQ_FUA) ?
			       "write+preflush+fua" :
			       "write+preflush");
	else if ((vio->bio->bi_opf & REQ_FUA) == REQ_FUA)
		description = "write+fua";
	else
		description = "write";

	update_vio_error_stats(vio,
			       "Completing %s vio of type %u for physical block %llu with error",
			       description,
			       vio->type,
			       (unsigned long long) pbn);
}

/**
 * make_vio_pool() - Create a new vio pool.
 * @vdo: The vdo.
 * @pool_size: The number of vios in the pool.
 * @thread_id: The ID of the thread using this pool.
 * @constructor: The constructor for vios in the pool.
 * @context: The context that each entry will have.
 * @pool_ptr: The resulting pool.
 *
 * Return: A success or error code.
 */
int make_vio_pool(struct vdo *vdo,
		  size_t pool_size,
		  thread_id_t thread_id,
		  vio_constructor *constructor,
		  void *context,
		  struct vio_pool **pool_ptr)
{
	struct vio_pool *pool;
	char *ptr;
	size_t i;

	int result = UDS_ALLOCATE_EXTENDED(struct vio_pool, pool_size,
					   struct vio_pool_entry, __func__,
					   &pool);
	if (result != VDO_SUCCESS)
		return result;

	pool->thread_id = thread_id;
	INIT_LIST_HEAD(&pool->available);
	INIT_LIST_HEAD(&pool->busy);

	result = UDS_ALLOCATE(pool_size * VDO_BLOCK_SIZE, char,
			      "VIO pool buffer", &pool->buffer);
	if (result != VDO_SUCCESS) {
		free_vio_pool(pool);
		return result;
	}

	ptr = pool->buffer;
	for (i = 0; i < pool_size; i++) {
		struct vio_pool_entry *entry = &pool->entries[i];

		entry->buffer = ptr;
		entry->context = context;
		result = constructor(vdo, entry, ptr, &entry->vio);
		if (result != VDO_SUCCESS) {
			free_vio_pool(pool);
			return result;
		}

		ptr += VDO_BLOCK_SIZE;
		INIT_LIST_HEAD(&entry->available_entry);
		list_add_tail(&entry->available_entry, &pool->available);
		pool->size++;
	}

	*pool_ptr = pool;
	return VDO_SUCCESS;
}

/**
 * free_vio_pool() - Destroy a vio pool.
 * @pool: The pool to free.
 */
void free_vio_pool(struct vio_pool *pool)
{
	struct vio_pool_entry *entry;
	size_t i;

	if (pool == NULL)
		return;

	/* Remove all available entries from the object pool. */
	ASSERT_LOG_ONLY(!has_waiters(&pool->waiting),
			"VIO pool must not have any waiters when being freed");
	ASSERT_LOG_ONLY((pool->busy_count == 0),
			"VIO pool must not have %zu busy entries when being freed",
			pool->busy_count);
	ASSERT_LOG_ONLY(list_empty(&pool->busy),
			"VIO pool must not have busy entries when being freed");

	while (!list_empty(&pool->available)) {
		entry = as_vio_pool_entry(pool->available.next);
		list_del_init(pool->available.next);
		free_vio(UDS_FORGET(entry->vio));
	}

	/* Make sure every vio_pool_entry has been removed. */
	for (i = 0; i < pool->size; i++) {
		struct bio *bio;

		entry = &pool->entries[i];
		if (list_empty(&entry->available_entry))
			continue;

		bio = entry->vio->bio;
		ASSERT_LOG_ONLY(list_empty(&entry->available_entry),
				"VIO Pool entry still in use: VIO is in use for physical block %llu for operation %u",
				(unsigned long long) pbn_from_vio_bio(bio),
				bio->bi_opf);
	}

	UDS_FREE(UDS_FORGET(pool->buffer));
	UDS_FREE(pool);
}

/**
 * is_vio_pool_busy() - Check whether an vio pool has outstanding entries.
 *
 * Return: true if the pool is busy.
 */
bool is_vio_pool_busy(struct vio_pool *pool)
{
	return (pool->busy_count != 0);
}

/**
 * acquire_vio_from_pool() - Acquire a vio and buffer from the pool
 *                           (asynchronous).
 * @pool: The vio pool.
 * @waiter: Object that is requesting a vio.
 */
void acquire_vio_from_pool(struct vio_pool *pool, struct waiter *waiter)
{
	struct list_head *entry;

	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"acquire from active vio_pool called from correct thread");

	if (list_empty(&pool->available)) {
		enqueue_waiter(&pool->waiting, waiter);
		return;
	}

	pool->busy_count++;
	entry = pool->available.next;
	list_move_tail(entry, &pool->busy);
	(*waiter->callback)(waiter, as_vio_pool_entry(entry));
}

/**
 * return_vio_to_pool() - Return a vio and its buffer to the pool.
 * @pool: The vio pool.
 * @entry: A vio pool entry.
 */
void return_vio_to_pool(struct vio_pool *pool, struct vio_pool_entry *entry)
{
	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"vio pool entry returned on same thread as it was acquired");
	entry->vio->completion.error_handler = NULL;
	if (has_waiters(&pool->waiting)) {
		notify_next_waiter(&pool->waiting, NULL, entry);
		return;
	}

	list_move_tail(&entry->available_entry, &pool->available);
	--pool->busy_count;
}
