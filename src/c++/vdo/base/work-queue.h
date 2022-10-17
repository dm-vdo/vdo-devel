/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_WORK_QUEUE_H
#define VDO_WORK_QUEUE_H

#include <linux/sched.h> /* for TASK_COMM_LEN */

#include "funnel-queue.h"

#include "types.h"

enum {
	MAX_VDO_WORK_QUEUE_NAME_LEN = TASK_COMM_LEN,
};

enum vdo_completion_priority {
	BIO_ACK_Q_ACK_PRIORITY = 0,
	BIO_ACK_Q_MAX_PRIORITY = 0,
	BIO_Q_COMPRESSED_DATA_PRIORITY = 0,
	BIO_Q_DATA_PRIORITY = 0,
	BIO_Q_FLUSH_PRIORITY = 2,
	BIO_Q_HIGH_PRIORITY = 2,
	BIO_Q_METADATA_PRIORITY = 1,
	BIO_Q_VERIFY_PRIORITY = 1,
	BIO_Q_MAX_PRIORITY = 2,
	CPU_Q_COMPLETE_VIO_PRIORITY = 0,
	CPU_Q_COMPLETE_READ_PRIORITY = 0,
	CPU_Q_COMPRESS_BLOCK_PRIORITY = 0,
	CPU_Q_EVENT_REPORTER_PRIORITY = 0,
	CPU_Q_HASH_BLOCK_PRIORITY = 0,
	CPU_Q_MAX_PRIORITY = 0,
	UDS_Q_PRIORITY = 0,
	UDS_Q_MAX_PRIORITY = 0,
	VDO_DEFAULT_Q_COMPLETION_PRIORITY = 1,
	VDO_DEFAULT_Q_FLUSH_PRIORITY = 2,
	VDO_DEFAULT_Q_MAP_BIO_PRIORITY = 0,
	VDO_DEFAULT_Q_SYNC_PRIORITY = 2,
	VDO_DEFAULT_Q_VIO_CALLBACK_PRIORITY = 1,
	VDO_DEFAULT_Q_MAX_PRIORITY = 2,
	/* The maximum allowable priority */
	VDO_WORK_Q_MAX_PRIORITY = 2,
	/* A value which must be out of range for a valid priority */
	VDO_WORK_Q_DEFAULT_PRIORITY = VDO_WORK_Q_MAX_PRIORITY + 1,
};

struct vdo_work_queue_type {
	void (*start)(void *);
	void (*finish)(void *);
	enum vdo_completion_priority max_priority;
	enum vdo_completion_priority default_priority;
};

struct vdo_completion;
struct vdo_thread;
struct vdo_work_queue;

int make_work_queue(const char *thread_name_prefix,
		    const char *name,
		    struct vdo_thread *owner,
		    const struct vdo_work_queue_type *type,
		    unsigned int thread_count,
		    void *thread_privates[],
		    struct vdo_work_queue **queue_ptr);

void enqueue_work_queue(struct vdo_work_queue *queue,
			struct vdo_completion *completion);

void finish_work_queue(struct vdo_work_queue *queue);

void free_work_queue(struct vdo_work_queue *queue);

void dump_work_queue(struct vdo_work_queue *queue);

void dump_completion_to_buffer(struct vdo_completion *completion,
			       char *buffer,
			       size_t length);

void *get_work_queue_private_data(void);
struct vdo_work_queue *get_current_work_queue(void);
struct vdo_thread *get_work_queue_owner(struct vdo_work_queue *queue);

bool __must_check
vdo_work_queue_type_is(struct vdo_work_queue *queue,
		       const struct vdo_work_queue_type *type);

#endif /* VDO_WORK_QUEUE_H */
