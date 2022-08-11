/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "permassert.h"
#include "uds-threads.h"

/**********************************************************************/
int uds_init_cond(struct cond_var *cond)
{
	int result = pthread_cond_init(&cond->condition, NULL);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_init error");
	return result;
}

/**********************************************************************/
int uds_signal_cond(struct cond_var *cond)
{
	int result = pthread_cond_signal(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_signal error");
	return result;
}

/**********************************************************************/
int uds_broadcast_cond(struct cond_var *cond)
{
	int result = pthread_cond_broadcast(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_broadcast error");
	return result;
}

/**********************************************************************/
int uds_wait_cond(struct cond_var *cond, struct mutex *mutex)
{
	int result = pthread_cond_wait(&cond->condition, &mutex->mutex);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_wait error");
	return result;
}

/**********************************************************************/
int uds_timed_wait_cond(struct cond_var *cond,
			struct mutex *mutex,
			ktime_t timeout)
{
	struct timespec ts = future_time(timeout);
	return pthread_cond_timedwait(&cond->condition, &mutex->mutex, &ts);
}

/**********************************************************************/
int uds_destroy_cond(struct cond_var *cond)
{
	int result = pthread_cond_destroy(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_destroy error");
	return result;
}
