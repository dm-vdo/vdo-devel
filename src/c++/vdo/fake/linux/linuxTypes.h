/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 *
 */

#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include <stdint.h>

typedef uint64_t sector_t;

#define BIT(nr) (((unsigned long) 1) << (nr))

#define GFP_KERNEL 1
#define GFP_NOWAIT 2

#define pgoff_t unsigned long

typedef struct {
	uint64_t val;
} pfn_t;

typedef unsigned int fmode_t;

#endif // LINUX_TYPES_H
