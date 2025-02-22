/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fake implementation of linux/lz4.h for unit tests.
 *
 * Copyright 2023 Red Hat
 */

#ifndef LINUX_LZ4_H
#define LINUX_LZ4_H

#include "../../tests/lz4.h"

#define LZ4_MEM_COMPRESS LZ4_context_size()
#define LZ4_ACCELERATION_DEFAULT 1

/**********************************************************************/
int LZ4_compress_default(const char *source,
                         char *dest,
                         int isize,
                         int maxOutputSize,
                         void *context);

/**********************************************************************/
int LZ4_decompress_safe(const char *source,
                        char *dest,
                        int isize,
                        int maxOutputSize);

#endif // LINUX_LZ4_H
