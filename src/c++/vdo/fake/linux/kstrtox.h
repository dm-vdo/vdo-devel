/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_KSTRTOX_H
#define LINUX_KSTRTOX_H

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

#include "types.h"

int
kstrtouint(const char *string, unsigned int base, unsigned int *result);
int
kstrtoull(const char *s, unsigned int base, uint64_t *res);

#endif // LINUX_KSTRTOX_H
