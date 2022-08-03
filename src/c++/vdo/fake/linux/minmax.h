/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/minmax.h.
 *
 * Copyright Red Hat
 *
 */

#ifndef LINUX_MINMAX_H
#define LINUX_MINMAX_H

#include "types.h"

#define __min(x, y) ((x) < (y) ? (x) : (y))

/**
 * min_t - return minimum of two values, using the specified type
 * @type: data type to use
 * @x: first value
 * @y: second value
 *
 * Note that, unlike in the kernel implementation, this form is not guaranteed
 * to evaluate to a constant expression, and is not safe to call on
 * non-constant inputs (such as x++). The kernel implementation runs afoul of
 * the compiler settings we use in user space.
 **/
#define min_t(type, x, y) ((type) __min((type) (x), (type) (y)))

#endif // LINUX_MINMAX_H
