/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 * $Id$
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

void timer_setup(struct timer_list *timer,
                 void (*func)(struct timer_list *),
                 uint32_t flags);

int mod_timer(struct timer_list *timer, unsigned long expires);

int del_timer_sync(struct timer_list *timer);

#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)


#endif /* LINUX_TIMER_H */
