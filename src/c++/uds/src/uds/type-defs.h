/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef TYPE_DEFS_H
#define TYPE_DEFS_H

/*
 * General system type definitions.
 */

#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#endif

typedef unsigned char byte;
#ifdef __KERNEL__

#define CHAR_BIT 8

#define INT64_MAX  (9223372036854775807L)
#define UCHAR_MAX  ((unsigned char)~0ul)
#define UINT8_MAX  ((uint8_t)~0ul)
#define UINT16_MAX ((uint16_t)~0ul)
#define UINT64_MAX ((uint64_t)~0ul)
#endif /* __KERNEL__ */
#endif /* TYPE_DEFS_H */
