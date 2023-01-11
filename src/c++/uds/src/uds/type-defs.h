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
#endif
#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/types.h>
#ifndef __KERNEL__
#include <stddef.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define sector_t u64

#define U8_MAX  ((u8)~0ul)
#define S8_MAX  ((s8)(U8_MAX >> 1))
#define U16_MAX ((u16)~0ul)
#define S16_MAX ((s16)(U16_MAX >> 1))
#define U32_MAX ((u32)~0ul)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define U64_MAX ((u64)~0ul)
#define S64_MAX ((s64)(U64_MAX >> 1))
#endif /* __KERNEL__ */
#endif /* TYPE_DEFS_H */
