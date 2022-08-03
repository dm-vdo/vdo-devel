/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Adapted from linux/kernel.h
 *
 * Copyright Red Hat
 *
 */

#ifndef LINUX_KERNEL_H
#define LINUX_KERNEL_H

#include "permassert.h"

/* generic data direction definitions */
#define READ  0
#define WRITE 1

#ifndef BUG_ON
#ifdef NDEBUG
#define BUG_ON(cond) do { if (cond) {} } while (0)
#else
#define BUG_ON(cond) ASSERT_LOG_ONLY(!(cond), "BUG_ON")
#endif
#endif
#define BUG()	BUG_ON(1)

#endif // LINUX_KERNEL_H
