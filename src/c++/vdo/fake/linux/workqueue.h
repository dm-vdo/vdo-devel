/*
 * FOR INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * $Id$
 */

#ifndef LINUX_WORKQUEUE_H
#define LINUX_WORKQUEUE_H

#include <linux/atomic.h>
#include <linux/list.h>

struct work_struct;

typedef void (* work_func_t)(struct work_struct *work);

struct work_struct {
	atomic64_t data;
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

static inline bool
cancel_work_sync(struct work_struct *work __attribute__((unused))) {
	return false;
}

#endif /* LINUX_WORKQUEUE_H */
