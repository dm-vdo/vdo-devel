/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 * THIS FILE IS A CANDIDATE FOR THE EVENTUAL UTILITY LIBRARY.
 */

#ifndef NUM_UTILS_H
#define NUM_UTILS_H

#include "numeric.h"

#include "types.h"

#ifdef __KERNEL__
#include <linux/log2.h>

#else
#include "permassert.h"

/**
 * is_power_of_2() - Return true if and only if a number is a power of two.
 */
static inline bool is_power_of_2(uint64_t n)
{
	return (n > 0) && ((n & (n - 1)) == 0);
}

/**
 * ilog2() - Efficiently calculate the base-2 logarithm of a number truncated
 *           to an integer value.
 * @n: The input value.
 *
 * This also happens to be the bit index of the highest-order non-zero bit in
 * the binary representation of the number, which can easily be used to
 * calculate the bit shift corresponding to a bit mask or an array capacity,
 * or to calculate the binary floor or ceiling (next lowest or highest power
 * of two).
 *
 * Return: The integer log2 of the value, or -1 if the value is zero.
 */
static inline int ilog2(uint64_t n)
{
	ASSERT_LOG_ONLY (n != 0, "ilog2() may not be passed 0");
	/*
	 * Many CPUs, including x86, directly support this calculation, so use
	 * the GCC function for counting the number of leading high-order zero
	 * bits.
	 */
	return 63 - __builtin_clzll(n);
}

#endif // !__KERNEL__
#endif /* NUM_UTILS_H */
