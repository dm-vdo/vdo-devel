/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include "common.h"

typedef uint64_t sector_t;

#define BIT(nr) (((unsigned long) 1) << (nr))

typedef unsigned int gfp_t;
#define GFP_KERNEL 1

#endif // LINUX_TYPES_H
