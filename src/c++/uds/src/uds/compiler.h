/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMMON_COMPILER_H
#define COMMON_COMPILER_H

#include <linux/compiler_attributes.h>
#ifdef __KERNEL__
#include <asm/rwonce.h>
#include <linux/compiler.h>
#endif
#include <linux/types.h>

#ifndef __KERNEL__
/*
 * Count the elements in a static array while attempting to catch some type errors. (See
 * http://stackoverflow.com/a/1598827 for an explanation.)
 */
#define ARRAY_SIZE(x) ((sizeof(x) / sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

#define container_of(ptr, type, member)                              \
	__extension__({                                              \
		__typeof__(((type *) 0)->member) * __mptr = (ptr);   \
		(type *) ((char *) __mptr - offsetof(type, member)); \
	})
#endif

#define const_container_of(ptr, type, member)                                     \
	__extension__({                                                           \
		const __typeof__(((type *) 0)->member) * __mptr = (ptr);          \
		(const type *) ((const char *) __mptr - offsetof(type, member));  \
	})

#ifndef __KERNEL__
/*
 * CPU Branch-prediction hints, courtesy of GCC. Defining these as inline functions instead of
 * macros spoils their magic, sadly.
 */
#define likely(expr) __builtin_expect(!!(expr), 1)
#define unlikely(expr) __builtin_expect(!!(expr), 0)
#endif

#ifndef __KERNEL__
#define MAX_ERRNO 4095

#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void * __must_check ERR_PTR(long error)
{
	return (void *) error;
}

static inline long __must_check PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

static inline bool __must_check IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}
#endif

#ifdef __KERNEL__
#define __STRING(x) #x
#endif

#ifndef __KERNEL__
#if __has_attribute(__fallthrough__)
#define fallthrough	__attribute__((__fallthrough__))
#else
#define fallthrough	do {} while (0)  /* fallthrough */
#endif
#endif

#endif /* COMMON_COMPILER_H */
