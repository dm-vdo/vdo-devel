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
	struct pooled_vio vios[];
};

static int allocate_vio_components(struct vdo *vdo,
				   enum vio_type vio_type,
				   enum vio_priority priority,
				   void *parent,
				   unsigned int block_count,
				   char *data,
				   struct vio *vio)
{
	struct bio *bio;
	int result;

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

	result = vdo_create_multi_block_bio(block_count, &bio);
	if (result != VDO_SUCCESS)
		return result;

	initialize_vio(vio,
		       bio,
		       block_count,
		       vio_type,
		       priority,
		       vdo);
	vio->completion.parent = parent;
	vio->data = data;
	return VDO_SUCCESS;
}

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
	int result;

	/*
	 * If struct vio grows past 256 bytes, we'll lose benefits of
	 * VDOSTORY-176.
	 */
	STATIC_ASSERT(sizeof(struct vio) <= 256);

	/*
	 * Metadata vios should use direct allocation and not use the buffer
	 * pool, which is reserved for submissions from the linux block layer.
	 */
	result = UDS_ALLOCATE(1, struct vio, __func__, &vio);
	if (result != VDO_SUCCESS) {
		uds_log_error("metadata vio allocation failure %d", result);
		return result;
	}

	result = allocate_vio_components(vdo,
					 vio_type,
					 priority,
					 parent,
					 block_count,
					 data,
					 vio);
	if (result != VDO_SUCCESS) {
		UDS_FREE(vio);
		return result;
	}

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
 * @vio_type: The type of vios in the pool.
 * @priority: The priority with which vios from the pool should be enqueued.
 * @context: The context that each entry will have.
 * @pool_ptr: The resulting pool.
 *
 * Return: A success or error code.
 */
int make_vio_pool(struct vdo *vdo,
		  size_t pool_size,
		  thread_id_t thread_id,
		  enum vio_type vio_type,
		  enum vio_priority priority,
		  void *context,
		  struct vio_pool **pool_ptr)
{
	struct vio_pool *pool;
	char *ptr;
	int result;

	result = UDS_ALLOCATE_EXTENDED(struct vio_pool,
				       pool_size,
				       struct pooled_vio,
				       __func__,
				       &pool);
	if (result != VDO_SUCCESS)
		return result;

	pool->thread_id = thread_id;
	INIT_LIST_HEAD(&pool->available);
	INIT_LIST_HEAD(&pool->busy);

	result = UDS_ALLOCATE(pool_size * VDO_BLOCK_SIZE,
			      char,
			      "VIO pool buffer",
			      &pool->buffer);
	if (result != VDO_SUCCESS) {
		free_vio_pool(pool);
		return result;
	}

	ptr = pool->buffer;
	for (pool->size = 0;
	     pool->size < pool_size;
	     pool->size++, ptr += VDO_BLOCK_SIZE) {
		struct pooled_vio *pooled = &pool->vios[pool->size];

		result = allocate_vio_components(vdo,
						 vio_type,
						 priority,
						 NULL,
						 1,
						 ptr,
						 &pooled->vio);
		if (result != VDO_SUCCESS) {
			free_vio_pool(pool);
			return result;
		}

		pooled->context = context;
		list_add_tail(&pooled->pool_entry, &pool->available);
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
	if (pool == NULL)
		return;

	/* Remove all available vios from the object pool. */
	ASSERT_LOG_ONLY(!has_waiters(&pool->waiting),
			"VIO pool must not have any waiters when being freed");
	ASSERT_LOG_ONLY((pool->busy_count == 0),
			"VIO pool must not have %zu busy entries when being freed",
			pool->busy_count);
	ASSERT_LOG_ONLY(list_empty(&pool->busy),
			"VIO pool must not have busy entries when being freed");

	while (!list_empty(&pool->available)) {
		struct pooled_vio *pooled = list_first_entry(&pool->available,
							     struct pooled_vio,
							     pool_entry);

		list_del(&pooled->pool_entry);
		vdo_free_bio(pooled->vio.bio);
		pool->size--;
	}

	ASSERT_LOG_ONLY(pool->size == 0,
			"VIO pool must not have missing entries when being freed");

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
	struct pooled_vio *pooled;

	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"acquire from active vio_pool called from correct thread");

	if (list_empty(&pool->available)) {
		enqueue_waiter(&pool->waiting, waiter);
		return;
	}

	pooled = list_first_entry(&pool->available,
				  struct pooled_vio,
				  pool_entry);
	pool->busy_count++;
	list_move_tail(&pooled->pool_entry, &pool->busy);
	(*waiter->callback)(waiter, pooled);
}

/**
 * return_vio_to_pool() - Return a vio to the pool
 * @pool: The vio pool.
 * @vio: The pooled vio to return.
 */
void return_vio_to_pool(struct vio_pool *pool, struct pooled_vio *vio)
{
	struct vdo_completion *completion = vio_as_completion(&vio->vio);

	ASSERT_LOG_ONLY((pool->thread_id == vdo_get_callback_thread_id()),
			"vio pool entry returned on same thread as it was acquired");

	completion->error_handler = NULL;
	completion->parent = NULL;
	if (has_waiters(&pool->waiting)) {
		notify_next_waiter(&pool->waiting, NULL, vio);
		return;
	}

	list_move_tail(&vio->pool_entry, &pool->available);
	--pool->busy_count;
}
