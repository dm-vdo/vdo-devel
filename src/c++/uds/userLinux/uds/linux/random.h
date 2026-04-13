/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef LINUX_RANDOM_H
#define LINUX_RANDOM_H

#include <linux/types.h>
#include <stddef.h>

void get_random_bytes(void *buffer, size_t byte_count);

static inline u32 get_random_u32(void) {
	u32 rand;

	get_random_bytes(&rand, sizeof(u32));
	return rand;
}

#endif /* LINUX_RANDOM_H */
