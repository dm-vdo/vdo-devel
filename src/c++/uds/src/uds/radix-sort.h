/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

#include "compiler.h"

struct radix_sorter;

int __must_check make_radix_sorter(unsigned int count,
				   struct radix_sorter **sorter);

void free_radix_sorter(struct radix_sorter *sorter);

int __must_check radix_sort(struct radix_sorter *sorter,
			    const unsigned char *keys[],
			    unsigned int count,
			    unsigned short length);

#endif /* RADIX_SORT_H */
