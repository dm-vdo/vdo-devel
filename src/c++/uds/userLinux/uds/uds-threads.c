/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "uds-threads.h"

#include <errno.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "syscalls.h"

enum {
	ONCE_NOT_DONE = 0,
	ONCE_IN_PROGRESS = 1,
	ONCE_COMPLETE = 2,
};

/**********************************************************************/
unsigned int uds_get_num_cores(void)
{
	cpu_set_t cpu_set;
	unsigned int n_cpus = 0;
	unsigned int i;

	if (sched_getaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
		uds_log_warning_strerror(errno,
					 "sched_getaffinity() failed, using 1 as number of cores.");
		return 1;
	}

	for (i = 0; i < CPU_SETSIZE; ++i) {
		n_cpus += CPU_ISSET(i, &cpu_set);
	}
	return n_cpus;
}

/**********************************************************************/
void uds_get_thread_name(char *name)
{
	process_control(PR_GET_NAME, (unsigned long) name, 0, 0, 0);
}

/**********************************************************************/
pid_t uds_get_thread_id(void)
{
	return syscall(SYS_gettid);
}

/*
 * Run the given function once only, and record that fact in the atomic value.
 */
void perform_once(atomic_t *once, void (*function)(void))
{
	for (;;) {
		switch (atomic_cmpxchg(once, ONCE_NOT_DONE, ONCE_IN_PROGRESS)) {
		case ONCE_NOT_DONE:
			function();
			atomic_set_release(once, ONCE_COMPLETE);
			return;
		case ONCE_IN_PROGRESS:
			sched_yield();
			break;
		case ONCE_COMPLETE:
			return;
		default:
			return;
		}
	}
}

struct thread_start_info {
	void (*thread_func)(void *);
	void *thread_data;
	const char *name;
};

/**********************************************************************/
static void *thread_starter(void *arg)
{
	struct thread_start_info *tsi = arg;
	void (*thread_func)(void *) = tsi->thread_func;
	void *thread_data = tsi->thread_data;

	/*
	 * The name is just advisory for humans examining it, so we don't
	 * care much if this fails.
	 */
	process_control(PR_SET_NAME, (unsigned long) tsi->name, 0, 0, 0);
	UDS_FREE(tsi);
	thread_func(thread_data);
	return NULL;
}

/**********************************************************************/
int uds_create_thread(void (*thread_func)(void *),
		      void *thread_data,
		      const char *name,
		      struct thread **new_thread)
{
	int result;
	struct thread_start_info *tsi;
	struct thread *thread;

	result = UDS_ALLOCATE(1, struct thread_start_info, __func__, &tsi);
	if (result != UDS_SUCCESS) {
		return result;
	}
	tsi->thread_func = thread_func;
	tsi->thread_data = thread_data;
	tsi->name = name;

	result = UDS_ALLOCATE(1, struct thread, __func__, &thread);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error allocating memory for %s", name);
		UDS_FREE(tsi);
		return result;
	}

	result = pthread_create(&thread->thread, NULL, thread_starter, tsi);
	if (result != 0) {
		result = -errno;
		uds_log_error_strerror(result, "could not create %s thread",
				       name);
		UDS_FREE(thread);
		UDS_FREE(tsi);
		return result;
	}

	*new_thread = thread;
	return UDS_SUCCESS;
}

/**********************************************************************/
int uds_join_threads(struct thread *th)
{
	int result;
	pthread_t pthread;

	result = pthread_join(th->thread, NULL);
	pthread = th->thread;
	UDS_FREE(th);
	ASSERT_LOG_ONLY((result == 0), "th: %p", (void *)pthread);
	return result;
}

/**********************************************************************/
int uds_initialize_barrier(struct barrier *barrier, unsigned int thread_count)
{
	int result;

        result = pthread_barrier_init(&barrier->barrier, NULL, thread_count);
	ASSERT_LOG_ONLY((result == 0), "pthread_barrier_init error");
	return result;
}

/**********************************************************************/
int uds_destroy_barrier(struct barrier *barrier)
{
	int result;

	result = pthread_barrier_destroy(&barrier->barrier);
	ASSERT_LOG_ONLY((result == 0), "pthread_barrier_destroy error");
	return result;
}

/**********************************************************************/
int uds_enter_barrier(struct barrier *barrier)
{
	int result;

	result = pthread_barrier_wait(&barrier->barrier);
	if (result == PTHREAD_BARRIER_SERIAL_THREAD) {
		return UDS_SUCCESS;
	}

	ASSERT_LOG_ONLY((result == 0),  "pthread_barrier_wait error");
	return result;
}
