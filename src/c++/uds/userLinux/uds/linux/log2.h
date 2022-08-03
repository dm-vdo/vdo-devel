/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef _LINUX_LOG2_H
#define _LINUX_LOG2_H

#include <stdbool.h>

/* Compute the number of bits to represent n */
static inline unsigned int bits_per(unsigned int n)
{
	unsigned int bits = 1;

	while (n > 1) {
		n >>= 1;
		bits++;
	}

	return bits;
}

#endif /* _LINUX_LOG2_H */
