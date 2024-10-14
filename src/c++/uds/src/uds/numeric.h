/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef UDS_NUMERIC_H
#define UDS_NUMERIC_H

#ifndef VDO_UPSTREAM
#include <linux/version.h>
#undef VDO_USE_NEXT
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_MINOR) && (RHEL_MINOR < 50)
#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(10, 0))
#define VDO_USE_NEXT
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0))
#define VDO_USE_NEXT
#endif
#endif /* !RHEL_RELEASE_CODE */
#endif /* !VDO_UPSTREAM */
#ifndef VDO_USE_NEXT
#include <asm/unaligned.h>
#else
#include <linux/unaligned.h>
#endif
#ifdef __KERNEL__
#include <linux/kernel.h>
#endif
#include <linux/types.h>

/*
 * These utilities encode or decode a number from an offset in a larger data buffer and then
 * advance the offset pointer to the next field in the buffer.
 */

static inline void decode_s64_le(const u8 *buffer, size_t *offset, s64 *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(s64);
}

static inline void encode_s64_le(u8 *data, size_t *offset, s64 to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(s64);
}

static inline void decode_u64_le(const u8 *buffer, size_t *offset, u64 *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(u64);
}

static inline void encode_u64_le(u8 *data, size_t *offset, u64 to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(u64);
}

static inline void decode_s32_le(const u8 *buffer, size_t *offset, s32 *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(s32);
}

static inline void encode_s32_le(u8 *data, size_t *offset, s32 to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(s32);
}

static inline void decode_u32_le(const u8 *buffer, size_t *offset, u32 *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(u32);
}

static inline void encode_u32_le(u8 *data, size_t *offset, u32 to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(u32);
}

static inline void decode_u16_le(const u8 *buffer, size_t *offset, u16 *decoded)
{
	*decoded = get_unaligned_le16(buffer + *offset);
	*offset += sizeof(u16);
}

static inline void encode_u16_le(u8 *data, size_t *offset, u16 to_encode)
{
	put_unaligned_le16(to_encode, data + *offset);
	*offset += sizeof(u16);
}

#endif /* UDS_NUMERIC_H */
