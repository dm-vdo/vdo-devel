/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef LINUX_PRANDOM_H
#define LINUX_PRANDOM_H

#include <stddef.h>

void prandom_bytes(void *buffer, size_t byte_count);

#endif /* LINUX_PRANDOM_H */
