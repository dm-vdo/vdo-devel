/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2025 Red Hat
 */

#ifndef STRING_H
#define STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <permassert.h>

static inline bool mem_is_zero(char *block, size_t size)
{
    int i;
    VDO_ASSERT_LOG_ONLY((uintptr_t)block % sizeof(u64) == 0,
                        "Data blocks are expected to be aligned to u64");
    VDO_ASSERT_LOG_ONLY(size % sizeof(u64) == 0,
                        "Data blocks are expected to be a multiple of u64");

    int count = size / sizeof(u64);
    for (i = 0; i < count; i++) {
        if (*((u64 *) &block[i * sizeof(u64)]))
            return false;
    }

    return true;
}

#endif /* STRING_H */
