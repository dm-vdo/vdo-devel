/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef NUMERIC_H
#define NUMERIC_H 1

#include <asm/unaligned.h>
#ifdef __KERNEL__
#include <linux/kernel.h>
#endif

#include "compiler.h"

/*
 * These utiltes encode or decode a number from an offset in a larger data
 * buffer and then advance the offset pointer to the next field in the buffer.
 */

static inline void decode_int64_le(const uint8_t *buffer,
				   size_t *offset,
				   int64_t *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(int64_t);
}

static inline void encode_int64_le(uint8_t *data,
				   size_t *offset,
				   int64_t to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(int64_t);
}

static inline void decode_uint64_le(const uint8_t *buffer,
				    size_t *offset,
				    uint64_t *decoded)
{
	*decoded = get_unaligned_le64(buffer + *offset);
	*offset += sizeof(uint64_t);
}

static inline void encode_uint64_le(uint8_t *data,
				    size_t *offset,
				    uint64_t to_encode)
{
	put_unaligned_le64(to_encode, data + *offset);
	*offset += sizeof(uint64_t);
}

static inline void decode_int32_le(const uint8_t *buffer,
				   size_t *offset,
				   int32_t *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(int32_t);
}

static inline void encode_int32_le(uint8_t *data,
				   size_t *offset,
				   int32_t to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(int32_t);
}

static inline void decode_uint32_le(const uint8_t *buffer,
				    size_t *offset,
				    uint32_t *decoded)
{
	*decoded = get_unaligned_le32(buffer + *offset);
	*offset += sizeof(uint32_t);
}

static inline void encode_uint32_le(uint8_t *data,
				    size_t *offset,
				    uint32_t to_encode)
{
	put_unaligned_le32(to_encode, data + *offset);
	*offset += sizeof(uint32_t);
}

static inline void decode_uint16_le(const uint8_t *buffer,
				    size_t *offset,
				    uint16_t *decoded)
{
	*decoded = get_unaligned_le16(buffer + *offset);
	*offset += sizeof(uint16_t);
}

static inline void encode_uint16_le(uint8_t *data,
				    size_t *offset,
				    uint16_t to_encode)
{
	put_unaligned_le16(to_encode, data + *offset);
	*offset += sizeof(uint16_t);
}

#endif /* NUMERIC_H */
