/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef UDS_THREADS_H
#define UDS_THREADS_H

#include <linux/atomic.h>
#ifdef __KERNEL__
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include "event-count.h"
#else
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#endif

#include "compiler.h"
#include "errors.h"
#include "time-utils.h"

/* Thread and synchronization utilities for UDS */

#ifdef __KERNEL__
struct cond_var {
	struct event_count *event_count;
};

struct thread;

struct barrier {
	/* Mutex for this barrier object */
	struct semaphore mutex;
	/* Semaphore for threads waiting at the barrier */
	struct semaphore wait;
	/* Number of threads which have arrived */
	int arrived;
	/* Total number of threads using this barrier */
	int thread_count;
};
#else
struct cond_var {
	pthread_cond_t condition;
};

struct mutex {
	pthread_mutex_t mutex;
};

struct semaphore {
	sem_t semaphore;
};

struct thread {
	pthread_t thread;
};

struct barrier {
	pthread_barrier_t barrier;
};

#ifndef NDEBUG
#define UDS_MUTEX_INITIALIZER PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
#else
#define UDS_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#endif

extern const bool UDS_DO_ASSERTIONS;
#endif

int __must_check uds_create_thread(void (*thread_function)(void *),
				   void *thread_data,
				   const char *name,
				   struct thread **new_thread);

unsigned int uds_get_num_cores(void);

pid_t __must_check uds_get_thread_id(void);
void perform_once(atomic_t *once_state, void (*function) (void));

int uds_join_threads(struct thread *thread);

int __must_check uds_initialize_barrier(struct barrier *barrier, unsigned int thread_count);
int uds_destroy_barrier(struct barrier *barrier);
int uds_enter_barrier(struct barrier *barrier);

int __must_check uds_init_cond(struct cond_var *cond);
int uds_signal_cond(struct cond_var *cond);
int uds_broadcast_cond(struct cond_var *cond);
int uds_wait_cond(struct cond_var *cond, struct mutex *mutex);
int uds_timed_wait_cond(struct cond_var *cond, struct mutex *mutex, ktime_t timeout);
int uds_destroy_cond(struct cond_var *cond);

#ifdef __KERNEL__
#ifdef TEST_INTERNAL
/* Apply a function to every thread that we have created. */
void uds_apply_to_threads(void apply_function(void *, struct task_struct *), void *argument);

/* This is a unit-test alternative to using BUG() or BUG_ON(). */
__attribute__((noreturn)) void uds_thread_exit(void);

#endif  /* TEST_INTERNAL */
static inline int __must_check uds_init_mutex(struct mutex *mutex)
{
	mutex_init(mutex);
	return UDS_SUCCESS;
}

static inline int uds_destroy_mutex(struct mutex *mutex)
{
	return UDS_SUCCESS;
}

static inline void uds_lock_mutex(struct mutex *mutex)
{
	mutex_lock(mutex);
}

static inline void uds_unlock_mutex(struct mutex *mutex)
{
	mutex_unlock(mutex);
}

static inline int __must_check
uds_initialize_semaphore(struct semaphore *semaphore, unsigned int value)
{
	sema_init(semaphore, value);
	return UDS_SUCCESS;
}

static inline int uds_destroy_semaphore(struct semaphore *semaphore)
{
	return UDS_SUCCESS;
}

static inline void uds_acquire_semaphore(struct semaphore *semaphore)
{
	/*
	 * Do not use down(semaphore). Instead use down_interruptible so that
	 * we do not get 120 second stall messages in kern.log.
	 */
	while (down_interruptible(semaphore) != 0)
		/*
		 * If we're called from a user-mode process (e.g., "dmsetup
		 * remove") while waiting for an operation that may take a
		 * while (e.g., UDS index save), and a signal is sent (SIGINT,
		 * SIGUSR2), then down_interruptible will not block. If that
		 * happens, sleep briefly to avoid keeping the CPU locked up in
		 * this loop. We could just call cond_resched, but then we'd
		 * still keep consuming CPU time slices and swamp other threads
		 * trying to do computational work. [VDO-4980]
		 */
		fsleep(1000);
}

static inline bool __must_check uds_attempt_semaphore(struct semaphore *semaphore, ktime_t timeout)
{
	unsigned int jiffies;

	if (timeout <= 0)
		return down_trylock(semaphore) == 0;

	jiffies = nsecs_to_jiffies(timeout);
	return down_timeout(semaphore, jiffies) == 0;
}

static inline void uds_release_semaphore(struct semaphore *semaphore)
{
	up(semaphore);
}
#else
void uds_get_thread_name(char *name);

static inline void cond_resched(void)
{
	/*
	 * On Linux sched_yield always succeeds so the result can be
	 * safely ignored.
	 */
	(void) sched_yield();
}

int uds_initialize_mutex(struct mutex *mutex, bool assert_on_error);
int __must_check uds_init_mutex(struct mutex *mutex);
int uds_destroy_mutex(struct mutex *mutex);
void uds_lock_mutex(struct mutex *mutex);
void uds_unlock_mutex(struct mutex *mutex);

int __must_check uds_initialize_semaphore(struct semaphore *semaphore, unsigned int value);
int uds_destroy_semaphore(struct semaphore *semaphore);
void uds_acquire_semaphore(struct semaphore *semaphore);
bool __must_check uds_attempt_semaphore(struct semaphore *semaphore, ktime_t timeout);
void uds_release_semaphore(struct semaphore *semaphore);
#endif /* __KERNEL__ */

#endif /* UDS_THREADS_H */
