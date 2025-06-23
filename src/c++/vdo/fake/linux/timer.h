/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test implementations of linux/timer.h.
 *
 * Copyright 2024 Red Hat
 */

#ifndef LINUX_TIMER_H
#define LINUX_TIMER_H

#include <linux/list.h>

/* Defined in linux/timer_types.h
 *
 * Fields which are not used in VDO unit tests are excluded.
 *
 * The entry field intentionally differs from the kernel, which is a hlist_node type.
 */
struct timer_list {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head entry;
	unsigned long expires;
	void (*function)(struct timer_list *);
	u32 flags;
};

void __init_timer(struct timer_list *timer,
		  void (*func)(struct timer_list *),
		  unsigned int flags);

#define timer_setup(timer, callback, flags)		\
	__init_timer((timer), (callback), (flags))

int mod_timer(struct timer_list *timer, unsigned long expires);

/* Renamed in Linux 6.15 kernel */
int del_timer_sync(struct timer_list *timer);
int timer_delete_sync(struct timer_list *timer);

#ifndef VDO_UPSTREAM
#undef VDO_USE_NEXT
#include <linux/version.h>
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_MINOR) && (RHEL_MINOR < 50)
#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(10, 1))
#define VDO_USE_NEXT
#endif
#else /* RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0))
#define VDO_USE_NEXT
#endif
#endif /* RHEL_RELEASE_CODE */
#endif /* VDO_UPSTREAM */
#ifndef VDO_USE_NEXT
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#else
#define timer_container_of(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#endif /* VDO_USE_NEXT */

#endif /* LINUX_TIMER_H */
