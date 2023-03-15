// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "uds-threads.h"

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/sched.h>

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

enum {
	ONCE_NOT_DONE = 0,
	ONCE_IN_PROGRESS = 1,
	ONCE_COMPLETE = 2,
};

/* Run a function once only, and record that fact in the atomic value. */
void uds_perform_once(atomic_t *once, void (*function)(void))
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
	uds_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_add_head(&thread->thread_links, &thread_list);
	mutex_unlock(&thread_mutex);
	uds_register_allocating_thread(&allocating_thread, NULL);
	thread->thread_function(thread->thread_data);
	uds_unregister_allocating_thread();
	complete(&thread->thread_done);
	return 0;
}

int uds_create_thread(void (*thread_function)(void *),
		      void *thread_data,
		      const char *name,
		      struct thread **new_thread)
{
	char *name_colon = strchr(name, ':');
	char *my_name_colon = strchr(current->comm, ':');
	struct task_struct *task;
	struct thread *thread;
	int result;

	result = UDS_ALLOCATE(1, struct thread, __func__, &thread);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error allocating memory for %s", name);
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
	if ((name_colon == NULL) && (my_name_colon != NULL))
		task = kthread_run(thread_starter,
				   thread,
				   "%.*s:%s",
				   (int) (my_name_colon - current->comm),
				   current->comm,
				   name);
	else
		task = kthread_run(thread_starter, thread, "%s", name);

	if (IS_ERR(task)) {
		UDS_FREE(thread);
		return PTR_ERR(task);
	}

	*new_thread = thread;
	return UDS_SUCCESS;
}

int uds_join_threads(struct thread *thread)
{
	while (wait_for_completion_interruptible(&thread->thread_done) != 0)
		/* empty loop */
		;

	mutex_lock(&thread_mutex);
	hlist_del(&thread->thread_links);
	mutex_unlock(&thread_mutex);
	UDS_FREE(thread);
	return UDS_SUCCESS;
}

#ifdef TEST_INTERNAL
void uds_apply_to_threads(void apply_function(void *, struct task_struct *), void *argument)
{
	struct thread *thread;

	uds_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_for_each_entry(thread, &thread_list, thread_links)
		apply_function(argument, thread->thread_task);
	mutex_unlock(&thread_mutex);
}

void uds_thread_exit(void)
{
	struct thread *thread;
	struct completion *completion = NULL;

	uds_perform_once(&thread_once, thread_init);
	mutex_lock(&thread_mutex);
	hlist_for_each_entry(thread, &thread_list, thread_links) {
		if (thread->thread_task == current) {
			completion = &thread->thread_done;
			break;
		}
	}
	mutex_unlock(&thread_mutex);
	uds_unregister_allocating_thread();

/*
 * Temporary workaround for LINUX_VERSION_CODE <= KERNEL_VERSION(5,17,0). We have two kernels, both
 * claiming to be version 5.17.0, that have different APIs. The only way to distinguish the two is
 * to check for the definition of a macro that was added as part of the change that implemented
 * kthread_complete_and_exit.
 */
#ifndef module_put_and_kthread_exit
	complete_and_exit(completion, 1);
#else
	kthread_complete_and_exit(completion, 1);
#endif
}

#endif /* TEST_INTERNAL */
pid_t uds_get_thread_id(void)
{
	return current->pid;
}

unsigned int uds_get_num_cores(void)
{
	return num_online_cpus();
}

int uds_initialize_barrier(struct barrier *barrier, unsigned int thread_count)
{
	int result;

	result = uds_initialize_semaphore(&barrier->mutex, 1);
	if (result != UDS_SUCCESS)
		return result;

	barrier->arrived = 0;
	barrier->thread_count = thread_count;
	return uds_initialize_semaphore(&barrier->wait, 0);
}

int uds_destroy_barrier(struct barrier *barrier)
{
	int result;

	result = uds_destroy_semaphore(&barrier->mutex);
	if (result != UDS_SUCCESS)
		return result;
	return uds_destroy_semaphore(&barrier->wait);
}

int uds_enter_barrier(struct barrier *barrier)
{
	bool last_thread;

	uds_acquire_semaphore(&barrier->mutex);
	last_thread = (++barrier->arrived == barrier->thread_count);
	if (last_thread) {
		int i;

		for (i = 1; i < barrier->thread_count; i++)
			uds_release_semaphore(&barrier->wait);

		barrier->arrived = 0;
		uds_release_semaphore(&barrier->mutex);
	} else {
		uds_release_semaphore(&barrier->mutex);
		uds_acquire_semaphore(&barrier->wait);
	}

	return UDS_SUCCESS;
}
