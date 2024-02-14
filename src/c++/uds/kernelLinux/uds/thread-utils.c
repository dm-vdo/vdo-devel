// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "thread-utils.h"

#include <asm/current.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/types.h>
#ifndef VDO_UPSTREAM
#include <linux/version.h>
#endif /* VDO_UPSTREAM */

#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"

static struct hlist_head thread_list;
static struct mutex thread_mutex;
static atomic_t thread_once = ATOMIC_INIT(0);

struct thread {
	void (*thread_function)(void *thread_data);
	void *thread_data;
	struct hlist_node thread_links;
	struct task_struct *thread_task;
	struct completion thread_done;
};

#define ONCE_NOT_DONE 0
#define ONCE_IN_PROGRESS 1
#define ONCE_COMPLETE 2

/* Run a function once only, and record that fact in the atomic value. */
void vdo_perform_once(atomic_t *once, void (*function)(void))
{
	for (;;) {
		switch (atomic_cmpxchg(once, ONCE_NOT_DONE, ONCE_IN_PROGRESS)) {
		case ONCE_NOT_DONE:
			function();
			atomic_set_release(once, ONCE_COMPLETE);
			return;
		case ONCE_IN_PROGRESS:
			cond_resched();
			break;
		case ONCE_COMPLETE:
			return;
		default:
			return;
		}
	}
}

static void thread_init(void)
{
	mutex_init(&thread_mutex);
}

static int thread_starter(void *arg)
{
	struct registered_thread allocating_thread;
	struct thread *thread = arg;

	thread->thread_task = current;
	vdo_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_add_head(&thread->thread_links, &thread_list);
	mutex_unlock(&thread_mutex);
	vdo_register_allocating_thread(&allocating_thread, NULL);
	thread->thread_function(thread->thread_data);
	vdo_unregister_allocating_thread();
	complete(&thread->thread_done);
	return 0;
}

int vdo_create_thread(void (*thread_function)(void *), void *thread_data,
		      const char *name, struct thread **new_thread)
{
	char *name_colon = strchr(name, ':');
	char *my_name_colon = strchr(current->comm, ':');
	struct task_struct *task;
	struct thread *thread;
	int result;

	result = vdo_allocate(1, struct thread, __func__, &thread);
	if (result != VDO_SUCCESS) {
		vdo_log_warning("Error allocating memory for %s", name);
		return result;
	}

	thread->thread_function = thread_function;
	thread->thread_data = thread_data;
	init_completion(&thread->thread_done);
	/*
	 * Start the thread, with an appropriate thread name.
	 *
	 * If the name supplied contains a colon character, use that name. This causes uds module
	 * threads to have names like "uds:callbackW" and the main test runner thread to be named
	 * "zub:runtest".
	 *
	 * Otherwise if the current thread has a name containing a colon character, prefix the name
	 * supplied with the name of the current thread up to (and including) the colon character.
	 * Thus when the "kvdo0:dedupeQ" thread opens an index session, all the threads associated
	 * with that index will have names like "kvdo0:foo".
	 *
	 * Otherwise just use the name supplied. This should be a rare occurrence.
	 */
	if ((name_colon == NULL) && (my_name_colon != NULL)) {
		task = kthread_run(thread_starter, thread, "%.*s:%s",
				   (int) (my_name_colon - current->comm), current->comm,
				   name);
	} else {
		task = kthread_run(thread_starter, thread, "%s", name);
	}

	if (IS_ERR(task)) {
		vdo_free(thread);
		return PTR_ERR(task);
	}

	*new_thread = thread;
	return VDO_SUCCESS;
}

void vdo_join_threads(struct thread *thread)
{
	while (wait_for_completion_interruptible(&thread->thread_done))
		fsleep(1000);

	mutex_lock(&thread_mutex);
	hlist_del(&thread->thread_links);
	mutex_unlock(&thread_mutex);
	vdo_free(thread);
}
#ifdef TEST_INTERNAL

void uds_apply_to_threads(void apply_function(void *, struct task_struct *),
			  void *argument)
{
	struct thread *thread;

	vdo_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_for_each_entry(thread, &thread_list, thread_links)
		apply_function(argument, thread->thread_task);
	mutex_unlock(&thread_mutex);
}

void uds_thread_exit(void)
{
	struct thread *thread;
	struct completion *completion = NULL;

	vdo_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_for_each_entry(thread, &thread_list, thread_links) {
		if (thread->thread_task == current) {
			completion = &thread->thread_done;
			break;
		}
	}
	mutex_unlock(&thread_mutex);
	vdo_unregister_allocating_thread();

#ifndef VDO_UPSTREAM
#undef VDO_USE_ALTERNATE
#ifdef RHEL_RELEASE_CODE
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9, 2))
#define VDO_USE_ALTERNATE
#endif
#else /* !RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0))
#define VDO_USE_ALTERNATE
#endif
#endif /* !RHEL_RELEASE_CODE */
#endif /* !VDO_UPSTREAM */
#ifdef VDO_USE_ALTERNATE
	complete_and_exit(completion, 1);
#else
	kthread_complete_and_exit(completion, 1);
#endif
}
#endif /* TEST_INTERNAL */
