/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Adapted from linux/kernel.h
 *
 * Copyright 2023 Red Hat
 *
 */

#ifndef LINUX_KERNEL_H
#define LINUX_KERNEL_H

#include "permassert.h"

/* generic data direction definitions */
#define READ  0
#define WRITE 1

#define WARN_ONCE(condition, format...) \
	(VDO_ASSERT_LOG_ONLY(!(condition), format) != UDS_SUCCESS)
#define WARN_ON_ONCE(condition) WARN_ONCE(condition)

#ifndef BUG_ON
#ifdef NDEBUG
#define BUG_ON(cond) do { if (cond) {} } while (0)
#else
#define BUG_ON(cond) VDO_ASSERT_LOG_ONLY(!(cond), "BUG_ON")
#endif
#endif
#define BUG()	BUG_ON(1)

#endif // LINUX_KERNEL_H
