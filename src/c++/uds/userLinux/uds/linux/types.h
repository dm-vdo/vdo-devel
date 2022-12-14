/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef UDS_LINUX_TYPES_H
#define UDS_LINUX_TYPES_H

/*
 * General system type definitions.
 */

#include <stdint.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;

typedef s8 __s8;
typedef u8 __u8;
typedef s16 __s16;
typedef u16 __u16;
typedef s32 __s32;
typedef u32 __u32;
typedef s64 __s64;
typedef u64 __u64;

#define __bitwise
typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

typedef unsigned int fmode_t;
#define FMODE_READ (fmode_t) 0x1
#define FMODE_WRITE (fmode_t) 0x2

#endif /* UDS_LINUX_TYPES_H */
