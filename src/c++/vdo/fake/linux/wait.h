/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test requirements from linux/wait.h.
 *
 * $Id$
 */
#ifndef LINUX_WAIT_H
#define LINUX_WAIT_H

#include <linux/list.h>
#include <linux/spinlock.h>

#include "uds-threads.h"

typedef struct wait_queue_head {
	spinlock_t		lock;
	struct list_head	head;
} wait_queue_head_t;

struct wait_queue_entry {
	void		       *private;
	struct list_head	entry;
};

#define DEFINE_WAIT(name)					\
  struct wait_queue_entry name = {                              \
    .private = current,						\
    .entry   = LIST_HEAD_INIT((name).entry),			\
  }

/**********************************************************************/
void init_waitqueue_head(wait_queue_head_t *head);

/**********************************************************************/
void prepare_to_wait_exclusive(wait_queue_head_t *queue,
			       struct wait_queue_entry *entry,
			       int state);

/**********************************************************************/
void finish_wait(wait_queue_head_t *queue, struct wait_queue_entry *entry);

/**********************************************************************/
void wake_up_nr(wait_queue_head_t *head, int32_t count);

#endif // LINUX_WAIT_H
