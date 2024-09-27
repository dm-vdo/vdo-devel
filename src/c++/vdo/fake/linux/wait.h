/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/wait.h.
 *
 * Copyright 2024 Red Hat
 *
 */

#ifndef LINUX_WAIT_H
#define LINUX_WAIT_H

#include <linux/list.h>
#include <linux/spinlock.h>

#include "thread-utils.h"

/* Fields which are not used in VDO unit tests are excluded. */
typedef struct wait_queue_head {
	spinlock_t		lock;
	struct list_head	head;
} wait_queue_head_t;

typedef struct wait_queue_entry {
	void                   *private;
	struct list_head	entry;
} wait_queue_entry_t;

#define DEFINE_WAIT(name)					\
  struct wait_queue_entry name = {                              \
    .private = current,						\
    .entry   = LIST_HEAD_INIT((name).entry),			\
  }

/**********************************************************************/
void init_waitqueue_head(struct wait_queue_head *wq_head);

/**********************************************************************/
void prepare_to_wait_exclusive(struct wait_queue_head *wq_head,
			       struct wait_queue_entry *wq_entry,
			       int state);

/**********************************************************************/
void finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry);

/**********************************************************************/
void __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr, void *key);

#define wake_up_nr(x, nr)	__wake_up(x, TASK_NORMAL, nr, NULL)

#endif // LINUX_WAIT_H
