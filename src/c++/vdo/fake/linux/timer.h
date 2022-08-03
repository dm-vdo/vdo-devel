/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_TIMER_H
#define LINUX_TIMER_H

#include <linux/jiffies.h>
#include <linux/list.h>

struct timer_list {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head entry;
	unsigned long expires;
	void (*function)(struct timer_list *);
	uint32_t flags;
};

// For now, these are all no-ops.
static inline void
timer_setup(struct timer_list *timer __attribute__((unused)),
	    void (*func)(struct timer_list *) __attribute__((unused)),
	    uint32_t flags  __attribute__((unused)))
{
}

static inline int mod_timer(struct timer_list *timer __attribute__((unused)),
			    unsigned long expires __attribute__((unused)))
{
}

static inline int
del_timer_sync(struct timer_list *timer __attribute__((unused)))
{
	return 0;
}

#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)


#endif /* LINUX_TIMER_H */
