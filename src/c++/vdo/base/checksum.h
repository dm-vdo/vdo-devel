/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <zlib.h>

/**
 * A function to calculate a CRC-32 checksum.
 *
 * @param buffer  The data to  checksum
 * @param length  The length of the data
 *
 * @return The checksum
 **/
static inline uint32_t vdo_crc32(const byte *buffer, size_t length)
{
	/*
	 * Different from the kernelspace wrapper in vdo.h, because the kernel
	 * implementation doesn't precondition or postcondition the data; the
	 * userspace implementation does. So, despite the difference in these
	 * two implementations, they actually do the same checksum.
	 */
	return crc32(~0L, buffer, length);
}
#endif /* CHECKSUM_H */
