/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 *
 */

#ifndef LINUX_KSTRTOX_H
#define LINUX_KSTRTOX_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

int __must_check kstrtoint(const char *string, unsigned int base, int *result);
int __must_check kstrtouint(const char *string, unsigned int base,
			    unsigned int *result);
int __must_check kstrtoull(const char *string, unsigned int base, u64 *result);

#endif // LINUX_KSTRTOX_H
