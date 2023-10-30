/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef UDS_MEMORY_ALLOC_H
#define UDS_MEMORY_ALLOC_H

#include <linux/cache.h>
#ifdef __KERNEL__
#include <linux/io.h> /* for PAGE_SIZE */
#else
#include <stdlib.h>
#include <string.h>
#endif

#include "permassert.h"
#ifdef __KERNEL__
#include "thread-registry.h"
#endif

/* Custom memory allocation functions for UDS that track memory usage */

int __must_check uds_allocate_memory(size_t size, size_t align, const char *what, void *ptr);

void uds_free_memory(void *ptr);

/* Free memory allocated with UDS_ALLOCATE(). */
#define UDS_FREE(PTR) uds_free_memory(PTR)

static inline void *uds_forget(void **ptr_ptr)
{
	void *ptr = *ptr_ptr;

	*ptr_ptr = NULL;
	return ptr;
}

/*
 * Null out a pointer and return a copy to it. This macro should be used when passing a pointer to
 * a function for which it is not safe to access the pointer once the function returns.
 */
#define UDS_FORGET(ptr) uds_forget((void **) &(ptr))

/*
 * Allocate storage based on element counts, sizes, and alignment.
 *
 * This is a generalized form of our allocation use case: It allocates an array of objects,
 * optionally preceded by one object of another type (i.e., a struct with trailing variable-length
 * array), with the alignment indicated.
 *
 * Why is this inline? The sizes and alignment will always be constant, when invoked through the
 * macros below, and often the count will be a compile-time constant 1 or the number of extra bytes
 * will be a compile-time constant 0. So at least some of the arithmetic can usually be optimized
 * away, and the run-time selection between allocation functions always can. In many cases, it'll
 * boil down to just a function call with a constant size.
 *
 * @count: The number of objects to allocate
 * @size: The size of an object
 * @extra: The number of additional bytes to allocate
 * @align: The required alignment
 * @what: What is being allocated (for error logging)
 * @ptr: A pointer to hold the allocated memory
 *
 * Return: UDS_SUCCESS or an error code
 */
static inline int uds_do_allocation(size_t count, size_t size, size_t extra,
				    size_t align, const char *what, void *ptr)
{
	size_t total_size = count * size + extra;

	/* Overflow check: */
	if ((size > 0) && (count > ((SIZE_MAX - extra) / size)))
		/*
		 * This is kind of a hack: We rely on the fact that SIZE_MAX would cover the entire
		 * address space (minus one byte) and thus the system can never allocate that much
		 * and the call will always fail. So we can report an overflow as "out of memory"
		 * by asking for "merely" SIZE_MAX bytes.
		 */
		total_size = SIZE_MAX;

	return uds_allocate_memory(total_size, align, what, ptr);
}

int __must_check uds_reallocate_memory(void *ptr, size_t old_size, size_t size,
				       const char *what, void *new_ptr);

/*
 * Allocate one or more elements of the indicated type, logging an error if the allocation fails.
 * The memory will be zeroed.
 *
 * @COUNT: The number of objects to allocate
 * @TYPE: The type of objects to allocate. This type determines the alignment of the allocation.
 * @WHAT: What is being allocated (for error logging)
 * @PTR: A pointer to hold the allocated memory
 *
 * Return: UDS_SUCCESS or an error code
 */
#define UDS_ALLOCATE(COUNT, TYPE, WHAT, PTR) \
	uds_do_allocation(COUNT, sizeof(TYPE), 0, __alignof__(TYPE), WHAT, PTR)

/*
 * Allocate one object of an indicated type, followed by one or more elements of a second type,
 * logging an error if the allocation fails. The memory will be zeroed.
 *
 * @TYPE1: The type of the primary object to allocate. This type determines the alignment of the
 *         allocated memory.
 * @COUNT: The number of objects to allocate
 * @TYPE2: The type of array objects to allocate
 * @WHAT: What is being allocated (for error logging)
 * @PTR: A pointer to hold the allocated memory
 *
 * Return: UDS_SUCCESS or an error code
 */
#define UDS_ALLOCATE_EXTENDED(TYPE1, COUNT, TYPE2, WHAT, PTR)            \
	__extension__({                                                  \
		int _result;						 \
		TYPE1 **_ptr = (PTR);                                    \
		BUILD_BUG_ON(__alignof__(TYPE1) < __alignof__(TYPE2));   \
		_result = uds_do_allocation(COUNT,                       \
					    sizeof(TYPE2),               \
					    sizeof(TYPE1),               \
					    __alignof__(TYPE1),          \
					    WHAT,                        \
					    _ptr);                       \
		_result;                                                 \
	})

/*
 * Allocate memory starting on a cache line boundary, logging an error if the allocation fails. The
 * memory will be zeroed.
 *
 * @size: The number of bytes to allocate
 * @what: What is being allocated (for error logging)
 * @ptr: A pointer to hold the allocated memory
 *
 * Return: UDS_SUCCESS or an error code
 */
static inline int __must_check uds_allocate_cache_aligned(size_t size, const char *what, void *ptr)
{
	return uds_allocate_memory(size, L1_CACHE_BYTES, what, ptr);
}

void *__must_check uds_allocate_memory_nowait(size_t size, const char *what);

/*
 * Allocate one element of the indicated type immediately, failing if the required memory is not
 * immediately available.
 *
 * @TYPE: The type of objects to allocate
 * @WHAT: What is being allocated (for error logging)
 *
 * Return: pointer to the memory, or NULL if the memory is not available.
 */
#define UDS_ALLOCATE_NOWAIT(TYPE, WHAT) uds_allocate_memory_nowait(sizeof(TYPE), WHAT)

int __must_check uds_duplicate_string(const char *string, const char *what,
				      char **new_string);

#ifdef TEST_INTERNAL
/* Wrapper which permits freeing a const pointer. */
static inline void uds_free_const(const void *pointer)
{
	union {
		const void *const_p;
		void *not_const;
	} u = { .const_p = pointer };
	UDS_FREE(u.not_const);
}

#endif /*TEST_INTERNAL */
#ifdef __KERNEL__
void uds_memory_exit(void);

void uds_memory_init(void);

void uds_register_allocating_thread(struct registered_thread *new_thread,
				    const bool *flag_ptr);

void uds_unregister_allocating_thread(void);

void uds_get_memory_stats(u64 *bytes_used, u64 *peak_bytes_used);

void uds_report_memory_usage(void);

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
/*
 * These are testing methods for injecting memory allocation errors. These methods are used by the
 * AllocFail UDSTests and by the MemoryFail VDOTests.
 */

extern atomic_long_t uds_allocate_memory_counter;
extern long uds_allocation_error_injection;

/*
 * Determine where a future memory allocation failure is scheduled.
 *
 * Return: true if there is a memory allocation failure scheduled to happen
 */
static inline bool uds_allocation_failure_scheduled(void)
{
	return atomic_long_read(&uds_allocate_memory_counter) < uds_allocation_error_injection;
}

/* Cancel any future memory allocation failure. */
static inline void cancel_uds_memory_allocation_failure(void)
{
	uds_allocation_error_injection = 0;
}

/*
 * Set up a future memory allocation failure. The first (count-1) allocations will succeed, and the
 * next one will fail with an -ENOMEM.
 *
 * @count: The number of the allocation that will fail
 */
static inline void schedule_uds_memory_allocation_failure(long count)
{
	uds_allocation_error_injection = atomic_long_read(&uds_allocate_memory_counter) + count;
}

/*
 * Control the recording of data tracking all memory allocations. If any such tracking is already
 * in progress, stop it so we can start afresh.
 *
 * @track_flag: True to begin tracking, or false to terminate tracking
 */
int track_uds_memory_allocations(bool track_flag);

/*
 * Log all the blocks that have been allocated but not freed since track_uds_memory_allocations()
 * was last called.
 */
void log_uds_memory_allocations(void);

#endif /* TEST_INTERNAL or VDO_INTERNAL */
#endif /* KERNEL */
#endif /* UDS_MEMORY_ALLOC_H */
