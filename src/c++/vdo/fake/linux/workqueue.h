/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test implementations of linux/workqueue.h.
 * 
 * Copyright 2024 Red Hat
 *
 */

#ifndef LINUX_WORKQUEUE_H
#define LINUX_WORKQUEUE_H

#include <linux/atomic.h>
#include <linux/list.h>

/* Defined in linux/workqueue_types.h */
struct work_struct;

typedef void (* work_func_t)(struct work_struct *work);

struct work_struct {
	atomic_long_t data;
	struct list_head entry;
	work_func_t func;
};

// For now, these are no-ops.

static inline void
INIT_WORK(struct work_struct *_work  __attribute__((unused)),
	  work_func_t _func __attribute__((unused)))
{
}

static inline bool
schedule_work(struct work_struct *work __attribute__((unused)))
{
	return true;
}

#endif /* LINUX_WORKQUEUE_H */
