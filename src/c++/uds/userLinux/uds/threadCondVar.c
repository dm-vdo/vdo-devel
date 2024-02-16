/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "permassert.h"
#include "thread-utils.h"

/**********************************************************************/
int uds_init_cond(struct cond_var *cond)
{
	int result;

	result = pthread_cond_init(&cond->condition, NULL);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_init error");
	return result;
}

/**********************************************************************/
void uds_signal_cond(struct cond_var *cond)
{
	int result;

	result = pthread_cond_signal(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_signal error");
}

/**********************************************************************/
void uds_broadcast_cond(struct cond_var *cond)
{
	int result;

	result = pthread_cond_broadcast(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_broadcast error");
}

/**********************************************************************/
void uds_wait_cond(struct cond_var *cond, struct mutex *mutex)
{
	int result;

	result = pthread_cond_wait(&cond->condition, &mutex->mutex);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_wait error");
}

#ifdef TEST_INTERNAL
/**********************************************************************/
int uds_timed_wait_cond(struct cond_var *cond,
			struct mutex *mutex,
			ktime_t timeout)
{
	struct timespec ts = future_time(timeout);

	return pthread_cond_timedwait(&cond->condition, &mutex->mutex, &ts);
}

#endif  /* TEST_INTERNAL */
/**********************************************************************/
void uds_destroy_cond(struct cond_var *cond)
{
	int result;

	result = pthread_cond_destroy(&cond->condition);
	ASSERT_LOG_ONLY((result == 0), "pthread_cond_destroy error");
}
