/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

#include "compiler.h"

/*
 * Radix sort is implemented using an American Flag sort, an unstable, in-place
 * 8-bit radix exchange sort. This is adapted from the algorithm in the paper
 * by Peter M. McIlroy, Keith Bostic, and M. Douglas McIlroy, "Engineering
 * Radix Sort".
 *
 * http://www.usenix.org/publications/compsystems/1993/win_mcilroy.pdf
 */

struct radix_sorter;

int __must_check make_radix_sorter(unsigned int count,
				   struct radix_sorter **sorter);

void free_radix_sorter(struct radix_sorter *sorter);

int __must_check radix_sort(struct radix_sorter *sorter,
			    const unsigned char *keys[],
			    unsigned int count,
			    unsigned short length);

#endif /* RADIX_SORT_H */
