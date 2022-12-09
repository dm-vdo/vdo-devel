// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "compiler.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
/*
 * This is more than just a wrapper around kmalloc and __vmalloc. Some of the additional code is
 * for testing and debugging only. This code is wrapped by the TEST_INTERNAL and VDO_INTERNAL
 * macros.
 */

#endif /* TEST_INTERNAL or VDO_INTERNAL */
/*
 * UDS and VDO keep track of which threads are allowed to allocate memory freely, and which threads
 * must be careful to not do a memory allocation that does an I/O request. The allocating_threads
 * threads_registry and its associated methods implement this tracking.
 */
static struct thread_registry allocating_threads;

static bool allocations_allowed(void)
{
	const bool *pointer = uds_lookup_thread(&allocating_threads);

	return (pointer != NULL) ? *pointer : false;
}

/*
 * Register the current thread as an allocating thread.
 *
 * An optional flag location can be supplied indicating whether, at any given point in time, the
 * threads associated with that flag should be allocating storage. If the flag is false, a message
 * will be logged.
 *
 * If no flag is supplied, the thread is always allowed to allocate storage without complaint.
 *
 * @new_thread: registered_thread structure to use for the current thread
 * @flag_ptr: Location of the allocation-allowed flag
 */
void uds_register_allocating_thread(struct registered_thread *new_thread, const bool *flag_ptr)
{
	if (flag_ptr == NULL) {
		static const bool allocation_always_allowed = true;

		flag_ptr = &allocation_always_allowed;
	}

	uds_register_thread(&allocating_threads, new_thread, flag_ptr);
}

/* Unregister the current thread as an allocating thread. */
void uds_unregister_allocating_thread(void)
{
	uds_unregister_thread(&allocating_threads);
}

/*
 * We track how much memory has been allocated and freed. When we unload the module, we log an
 * error if we have not freed all the memory that we allocated. Nearly all memory allocation and
 * freeing is done using this module.
 *
 * We do not use kernel functions like the kvasprintf() method, which allocate memory indirectly
 * using kmalloc.
 *
 * These data structures and methods are used to track the amount of memory used.
 */

/*
 * We allocate very few large objects, and allocation/deallocation isn't done in a
 * performance-critical stage for us, so a linked list should be fine.
 */
struct vmalloc_block_info {
	void *ptr;
	size_t size;
	struct vmalloc_block_info *next;
};

static struct {
	spinlock_t lock;
	size_t kmalloc_blocks;
	size_t kmalloc_bytes;
	size_t vmalloc_blocks;
	size_t vmalloc_bytes;
	size_t peak_bytes;
	struct vmalloc_block_info *vmalloc_list;
} memory_stats __cacheline_aligned;

static void update_peak_usage(void)
{
	size_t total_bytes = memory_stats.kmalloc_bytes + memory_stats.vmalloc_bytes;

	if (total_bytes > memory_stats.peak_bytes)
		memory_stats.peak_bytes = total_bytes;
}

static void add_kmalloc_block(size_t size)
{
	unsigned long flags;

	spin_lock_irqsave(&memory_stats.lock, flags);
	memory_stats.kmalloc_blocks++;
	memory_stats.kmalloc_bytes += size;
	update_peak_usage();
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

static void remove_kmalloc_block(size_t size)
{
	unsigned long flags;

	spin_lock_irqsave(&memory_stats.lock, flags);
	memory_stats.kmalloc_blocks--;
	memory_stats.kmalloc_bytes -= size;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

static void add_vmalloc_block(struct vmalloc_block_info *block)
{
	unsigned long flags;

	spin_lock_irqsave(&memory_stats.lock, flags);
	block->next = memory_stats.vmalloc_list;
	memory_stats.vmalloc_list = block;
	memory_stats.vmalloc_blocks++;
	memory_stats.vmalloc_bytes += block->size;
	update_peak_usage();
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

static void remove_vmalloc_block(void *ptr)
{
	struct vmalloc_block_info *block;
	struct vmalloc_block_info **block_ptr;
	unsigned long flags;

	spin_lock_irqsave(&memory_stats.lock, flags);
	for (block_ptr = &memory_stats.vmalloc_list;
	     (block = *block_ptr) != NULL;
	     block_ptr = &block->next) {
		if (block->ptr == ptr) {
			*block_ptr = block->next;
			memory_stats.vmalloc_blocks--;
			memory_stats.vmalloc_bytes -= block->size;
			break;
		}
	}

	spin_unlock_irqrestore(&memory_stats.lock, flags);
	if (block != NULL)
		UDS_FREE(block);
	else
		uds_log_info("attempting to remove ptr %px not found in vmalloc list", ptr);
}

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
/*
 * We use these global variables to control the insertion of memory allocation faults during
 * testing. The primary purpose here is to test error code paths.
 *
 * UDS tests use schedule_uds_memory_allocation_failure() (and its related methods) to directly
 * control memory allocation faults.
 *
 * VDO tests use the sysfs node /sys/uds/memory/schedule_allocation_failure (and its related nodes)
 * from the perl testing code to control memory allocation faults.
 *
 * The atomic uds_allocate_memory_counter counts the number of uds_allocate_memory() calls that
 * have a non-zero size. When any memory allocation increments the counter and it is equal to the
 * value of uds_allocation_error_injection, that allocation request will fail. This facility is for
 * testing only, and depends upon a "long" being large enough that wraparound is not a problem.
 */
atomic_long_t uds_allocate_memory_counter = ATOMIC_LONG_INIT(0);
long uds_allocation_error_injection;

/*
 * We use these variables and methods to track the exact blocks that are allocated.
 *
 * Tests can track memory by using track_uds_memory_allocations() and log_uds_memory_allocations().
 * When a test tries to provoke a memory leak and detects one, the test can log the blocks that are
 * yet to be freed.
 */
struct track_block_info {
	void *ptr;
	const char *what;
	size_t size;
};

struct track_memory_info {
	struct track_memory_info *next;
	int count;
	struct track_block_info blocks[];
};

/* Compute how many track_block_info will fit in a single page of memory. */
enum { NUM_TRACK_BLOCKS = ((PAGE_SIZE - sizeof(struct track_memory_info)) /
			   sizeof(struct track_block_info)) };

static struct mutex track_mutex;
static struct track_memory_info *track_info;
static atomic_t track_enabled = ATOMIC_INIT(0);
static bool track_always;

int track_uds_memory_allocations(bool track_flag)
{
	mutex_lock(&track_mutex);
	while (track_info != NULL) {
		struct track_memory_info *info = track_info;

		track_info = track_info->next;
		kfree(info);
	}
	atomic_set(&track_enabled, track_flag ? 1 : 0);
	track_always = false;
	mutex_unlock(&track_mutex);
	return UDS_SUCCESS;
}

static void add_tracking_block(void *ptr, size_t size, const char *what)
{
	struct track_memory_info *info;

	if (atomic_read(&track_enabled) == 0)
		return;

	mutex_lock(&track_mutex);
	/* Find a page with an available slot. */
	for (info = track_info; info != NULL; info = info->next)
		if (info->count < NUM_TRACK_BLOCKS)
			break;

	if (info == NULL) {
		/* All pages are full, so allocate a new one. */
		info = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (info != NULL) {
			info->next = track_info;
			info->count = 0;
			track_info = info;
		}
	}

	if (info == NULL) {
		/* We still don't have a page, just forget this block. */
		uds_log_warning("Could not allocate for memory tracking");
		track_always = false;
	} else {
		int index = info->count++;
		struct track_block_info *block = &info->blocks[index];

		block->ptr = ptr;
		block->size = size;
		block->what = what;
	}

	mutex_unlock(&track_mutex);
}

static void remove_tracking_block(void *ptr)
{
	struct track_memory_info *info;
	int i;

	if (atomic_read(&track_enabled) == 0)
		return;

	mutex_lock(&track_mutex);
	for (info = track_info; info != NULL; info = info->next) {
		for (i = 0; i < info->count; i++) {
			if (info->blocks[i].ptr == ptr) {
				info->blocks[i] = info->blocks[--info->count];
				mutex_unlock(&track_mutex);
				return;
			}
		}
	}

	mutex_unlock(&track_mutex);
	ASSERT_LOG_ONLY(!track_always, "UDS_FREE called on block that UDS did not UDS_ALLOCATE");
}

void log_uds_memory_allocations(void)
{
	int count = 0;
	int max_count = 0;
	int i;
	struct track_memory_info *info;

	if (atomic_read(&track_enabled) == 0)
		return;

	mutex_lock(&track_mutex);
	for (info = track_info; info != NULL; info = info->next) {
		count += info->count;
		max_count += NUM_TRACK_BLOCKS;
	}

	uds_log_info("Using %d of %d blocks", count, max_count);
	for (info = track_info; info != NULL; info = info->next) {
		for (i = 0; i < info->count; i++) {
			struct track_block_info *block = &info->blocks[i];

			uds_log_info("  %zu bytes for %s", block->size, block->what);
		}
	}

	mutex_unlock(&track_mutex);
}

#endif /* TEST_INTERNAL or VDO_INTERNAL */
/*
 * Determine whether allocating a memory block should use kmalloc or __vmalloc.
 *
 * vmalloc can allocate any integral number of pages.
 *
 * kmalloc can allocate any number of bytes up to a configured limit, which defaults to 8 megabytes
 * on some systems. kmalloc is especially good when memory is being both allocated and freed, and
 * it does this efficiently in a multi CPU environment.
 *
 * kmalloc usually rounds the size of the block up to the next power of two, so when the requested
 * block is bigger than PAGE_SIZE / 2 bytes, kmalloc will never give you less space than the
 * corresponding vmalloc allocation. Sometimes vmalloc will use less overhead than kmalloc.
 *
 * The advantages of kmalloc do not help out UDS or VDO, because we allocate all our memory up
 * front and do not free and reallocate it. Sometimes we have problems using kmalloc, because the
 * Linux memory page map can become so fragmented that kmalloc will not give us a 32KB chunk. We
 * have used vmalloc as a backup to kmalloc in the past, and a follow-up vmalloc of 32KB will work.
 * But there is no strong case to be made for using kmalloc over vmalloc for these size chunks.
 *
 * The kmalloc/vmalloc boundary is set at 4KB, and kmalloc gets the 4KB requests. There is no
 * strong reason for favoring either kmalloc or vmalloc for 4KB requests, except that tracking
 * vmalloc statistics uses a linked list implementation. Using a simple test, this choice of
 * boundary results in 132 vmalloc calls. Using vmalloc for requests of exactly 4KB results in an
 * additional 6374 vmalloc calls, which is much less efficient for tracking.
 *
 * @size: How many bytes to allocate
 */
static inline bool use_kmalloc(size_t size)
{
	return size <= PAGE_SIZE;
}

/*
 * Allocate storage based on memory size and alignment, logging an error if the allocation fails.
 * The memory will be zeroed.
 *
 * @size: The size of an object
 * @align: The required alignment
 * @what: What is being allocated (for error logging)
 * @ptr: A pointer to hold the allocated memory
 *
 * Return: UDS_SUCCESS or an error code
 */
int uds_allocate_memory(size_t size, size_t align, const char *what, void *ptr)
{
	/*
	 * The __GFP_RETRY_MAYFAIL flag means the VM implementation will retry memory reclaim
	 * procedures that have previously failed if there is some indication that progress has
	 * been made elsewhere. It can wait for other tasks to attempt high level approaches to
	 * freeing memory such as compaction (which removes fragmentation) and page-out. There is
	 * still a definite limit to the number of retries, but it is a larger limit than with
	 * __GFP_NORETRY. Allocations with this flag may fail, but only when there is genuinely
	 * little unused memory. While these allocations do not directly trigger the OOM killer,
	 * their failure indicates that the system is likely to need to use the OOM killer soon.
	 * The caller must handle failure, but can reasonably do so by failing a higher-level
	 * request, or completing it only in a much less efficient manner.
	 */
	const gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_RETRY_MAYFAIL;
	unsigned int noio_flags;
	bool allocations_restricted = !allocations_allowed();
	unsigned long start_time;
	void *p = NULL;

	if (ptr == NULL)
		return UDS_INVALID_ARGUMENT;

	if (size == 0) {
		*((void **) ptr) = NULL;
		return UDS_SUCCESS;
	}

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	if (atomic_long_inc_return(&uds_allocate_memory_counter) ==
	    uds_allocation_error_injection) {
		uds_log_warning("Injecting %s error on %zu bytes for %s", __func__, size, what);
		uds_log_backtrace(UDS_LOG_WARNING);
		return -ENOMEM;
	}

#endif /* TEST_INTERNAL or VDO_INTERNAL */
	if (allocations_restricted)
		noio_flags = memalloc_noio_save();

	start_time = jiffies;
	if (use_kmalloc(size) && (align < PAGE_SIZE)) {
		p = kmalloc(size, gfp_flags | __GFP_NOWARN);
		if (p == NULL) {
			/*
			 * It is possible for kmalloc to fail to allocate memory because there is
			 * no page available (see VDO-3688). A short sleep may allow the page
			 * reclaimer to free a page.
			 */
			fsleep(1000);
			p = kmalloc(size, gfp_flags);
		}

		if (p != NULL)
			add_kmalloc_block(ksize(p));
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
		if (p != NULL)
			add_tracking_block(p, ksize(p), what);
#endif /* TEST_INTERNAL or VDO_INTERNAL */
	} else {
		struct vmalloc_block_info *block;

		if (UDS_ALLOCATE(1, struct vmalloc_block_info, __func__, &block) ==
		    UDS_SUCCESS) {
			/*
			 * It is possible for __vmalloc to fail to allocate memory because there
			 * are no pages available (see VDO-3661). A short sleep may allow the page
			 * reclaimer to free enough pages for a small allocation.
			 *
			 * For larger allocations, the page_alloc code is racing against the page
			 * reclaimer. If the page reclaimer can stay ahead of page_alloc, the
			 * __vmalloc will succeed. But if page_alloc overtakes the page reclaimer,
			 * the allocation fails. It is possible that more retries will succeed.
			 */
			for (;;) {
				p = __vmalloc(size, gfp_flags | __GFP_NOWARN);

				if (p != NULL)
					break;

				if (jiffies_to_msecs(jiffies - start_time) > 1000) {
					/* Try one more time, logging a failure for this call. */
					p = __vmalloc(size, gfp_flags);
					break;
				}

				fsleep(1000);
			}

			if (p == NULL) {
				UDS_FREE(block);
			} else {
				block->ptr = p;
				block->size = PAGE_ALIGN(size);
				add_vmalloc_block(block);
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
				add_tracking_block(p, block->size, what);
#endif /* TEST_INTERNAL or VDO_INTERNAL */
			}
		}
	}

	if (allocations_restricted)
		memalloc_noio_restore(noio_flags);

	if (p == NULL) {
		unsigned int duration = jiffies_to_msecs(jiffies - start_time);

		uds_log_error("Could not allocate %zu bytes for %s in %u msecs",
			      size,
			      what,
			      duration);
		return -ENOMEM;
	}

	*((void **) ptr) = p;
	return UDS_SUCCESS;
}

/*
 * Allocate storage based on memory size, failing immediately if the required memory is not
 * available. The memory will be zeroed.
 *
 * @size: The size of an object.
 * @what: What is being allocated (for error logging)
 *
 * Return: pointer to the allocated memory, or NULL if the required space is not available.
 */
void *uds_allocate_memory_nowait(size_t size, const char *what __maybe_unused)
{
	void *p = kmalloc(size, GFP_NOWAIT | __GFP_ZERO);

	if (p != NULL)
		add_kmalloc_block(ksize(p));
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	if (p != NULL)
		/*
		 * This call may possibly do an allocation without GFP_NOWAIT. Since this code path
		 * is only used for memory allocation testing, we can live with the possibility of
		 * failure here.
		 */
		add_tracking_block(p, ksize(p), what);
#endif /* TEST_INTERNAL or VDO_INTERNAL */

	return p;
}

void uds_free_memory(void *ptr)
{
	if (ptr != NULL) {
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
		remove_tracking_block(ptr);
#endif /* TEST_INTERNAL or VDO_INTERNAL */
		if (is_vmalloc_addr(ptr)) {
			remove_vmalloc_block(ptr);
			vfree(ptr);
		} else {
			remove_kmalloc_block(ksize(ptr));
			kfree(ptr);
		}
	}
}

/*
 * Reallocate dynamically allocated memory. There are no alignment guarantees for the reallocated
 * memory. If the new memory is larger than the old memory, the new space will be zeroed.
 *
 * @ptr: The memory to reallocate.
 * @old_size: The old size of the memory
 * @size: The new size to allocate
 * @what: What is being allocated (for error logging)
 * @new_ptr: A pointer to hold the reallocated pointer
 *
 * Return: UDS_SUCCESS or an error code
 */
int uds_reallocate_memory(void *ptr, size_t old_size, size_t size, const char *what, void *new_ptr)
{
	int result;

	if (size == 0) {
		UDS_FREE(ptr);
		*(void **) new_ptr = NULL;
		return UDS_SUCCESS;
	}

	result = UDS_ALLOCATE(size, char, what, new_ptr);
	if (result != UDS_SUCCESS)
		return result;

	if (ptr != NULL) {
		if (old_size < size)
			size = old_size;

		memcpy(*((void **) new_ptr), ptr, size);
		UDS_FREE(ptr);
	}

	return UDS_SUCCESS;
}

int uds_duplicate_string(const char *string, const char *what, char **new_string)
{
	int result;
	byte *dup;

	result = UDS_ALLOCATE(strlen(string) + 1, byte, what, &dup);
	if (result != UDS_SUCCESS)
		return result;

	memcpy(dup, string, strlen(string) + 1);
	*new_string = dup;
	return UDS_SUCCESS;
}

void uds_memory_init(void)
{
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	mutex_init(&track_mutex);

#endif /* TEST_INTERNAL or VDO_INTERNAL */
	spin_lock_init(&memory_stats.lock);
	uds_initialize_thread_registry(&allocating_threads);
}

void uds_memory_exit(void)
{
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	track_uds_memory_allocations(false);

#endif /* TEST_INTERNAL or VDO_INTERNAL */
	ASSERT_LOG_ONLY(memory_stats.kmalloc_bytes == 0,
			"kmalloc memory used (%zd bytes in %zd blocks) is returned to the kernel",
			memory_stats.kmalloc_bytes,
			memory_stats.kmalloc_blocks);
	ASSERT_LOG_ONLY(memory_stats.vmalloc_bytes == 0,
			"vmalloc memory used (%zd bytes in %zd blocks) is returned to the kernel",
			memory_stats.vmalloc_bytes,
			memory_stats.vmalloc_blocks);
	uds_log_debug("peak usage %zd bytes", memory_stats.peak_bytes);
}

void get_uds_memory_stats(uint64_t *bytes_used, uint64_t *peak_bytes_used)
{
	unsigned long flags;

	spin_lock_irqsave(&memory_stats.lock, flags);
	*bytes_used = memory_stats.kmalloc_bytes + memory_stats.vmalloc_bytes;
	*peak_bytes_used = memory_stats.peak_bytes;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

/*
 * Report stats on any allocated memory that we're tracking. Not all allocation types are
 * guaranteed to be tracked in bytes (e.g., bios).
 */
void report_uds_memory_usage(void)
{
	unsigned long flags;
	uint64_t kmalloc_blocks;
	uint64_t kmalloc_bytes;
	uint64_t vmalloc_blocks;
	uint64_t vmalloc_bytes;
	uint64_t peak_usage;
	uint64_t total_bytes;

	spin_lock_irqsave(&memory_stats.lock, flags);
	kmalloc_blocks = memory_stats.kmalloc_blocks;
	kmalloc_bytes = memory_stats.kmalloc_bytes;
	vmalloc_blocks = memory_stats.vmalloc_blocks;
	vmalloc_bytes = memory_stats.vmalloc_bytes;
	peak_usage = memory_stats.peak_bytes;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
	total_bytes = kmalloc_bytes + vmalloc_bytes;
	uds_log_info("current module memory tracking (actual allocation sizes, not requested):");
	uds_log_info("  %llu bytes in %llu kmalloc blocks",
		     (unsigned long long) kmalloc_bytes,
		     (unsigned long long) kmalloc_blocks);
	uds_log_info("  %llu bytes in %llu vmalloc blocks",
		     (unsigned long long) vmalloc_bytes,
		     (unsigned long long) vmalloc_blocks);
	uds_log_info("  total %llu bytes, peak usage %llu bytes",
		     (unsigned long long) total_bytes,
		     (unsigned long long) peak_usage);
}
