/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#endif

#include "compiler.h"
#include "type-defs.h"

static INLINE const char *uds_bool_to_string(bool value)
{
	return (value ? "true" : "false");
}

#if !defined(__KERNEL__) || defined(TEST_INTERNAL)
int __must_check uds_alloc_sprintf(const char *what,
				   char **strp,
				   const char *fmt, ...)
	__printf(3, 4);

#endif /* (! __KERNEL) or TEST_INTERNAL */
#ifdef TEST_INTERNAL
int __must_check uds_fixed_sprintf(char *buf,
				   size_t buf_size,
				   const char *fmt, ...)
	__printf(3, 4);

#endif /* TEST_INTERNAL */
char *uds_append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
	__printf(3, 4);

#endif /* STRING_UTILS_H */
