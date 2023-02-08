/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RANDOM_H
#define RANDOM_H

#include <linux/random.h>

#define RAND_MAX 2147483647

static inline long random(void)
{
	long value;
	get_random_bytes(&value, sizeof(value));
	return value & RAND_MAX;
}

#endif /* RANDOM_H */
