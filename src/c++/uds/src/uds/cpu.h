/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef CPU_H
#define CPU_H

#include <linux/cache.h>

#include "compiler.h"

/**
 * Minimize cache-miss latency by moving data into a CPU cache before it is accessed.
 *
 * @address: the address to fetch (may be invalid)
 * @for_write: must be constant at compile time--false if for reading, true if for writing
 **/
static inline void prefetch_address(const void *address, bool for_write)
{
	/*
	 * for_write won't be a constant if we are compiled with optimization turned off, in which
	 * case prefetching really doesn't matter. clang can't figure out that if for_write is a
	 * constant, it can be passed as the second, mandatorily constant argument to prefetch(),
	 * at least currently on llvm 12.
	 */
	if (__builtin_constant_p(for_write)) {
		if (for_write)
			__builtin_prefetch(address, true);
		else
			__builtin_prefetch(address, false);
	}
}

/**
 * Minimize cache-miss latency by moving a range of addresses into a CPU cache before they are
 * accessed.
 *
 * @start: the starting address to fetch (may be invalid)
 * @size: the number of bytes in the address range
 * @for_write: must be constant at compile time--false if for reading, true if for writing
 **/
static inline void
prefetch_range(const void *start, unsigned int size, bool for_write)
{
	/*
	 * Count the number of cache lines to fetch, allowing for the address range to span an
	 * extra cache line boundary due to address alignment.
	 */
	const char *address = (const char *) start;
	unsigned int offset = ((uintptr_t) address % L1_CACHE_BYTES);
	unsigned int cache_lines = (1 + ((size + offset) / L1_CACHE_BYTES));

	while (cache_lines-- > 0) {
		prefetch_address(address, for_write);
		address += L1_CACHE_BYTES;
	}
}

#endif /* CPU_H */
