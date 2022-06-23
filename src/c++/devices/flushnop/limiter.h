/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef LIMITER_H
#define LIMITER_H

#include <linux/wait.h>

/*
 * A Limiter is a fancy counter used to limit resource usage.  We have a
 * limit to number of resources that we are willing to use, and a Limiter
 * holds us to that limit.
 */

typedef struct limiter {
  // A spinlock controlling access to the contents of this struct
  spinlock_t        lock;
  // The queue of threads waiting for a resource to become available
  wait_queue_head_t waiterQueue;
  // The number of resources in use
  uint32_t          active;
  // The maximum number number of resources that have ever been in use
  uint32_t          maximum;
  // The limit to the number of resources that are allowed to be used
  uint32_t          limit;
} Limiter;

/**
 * Get the Limiter variable values (atomically under the lock)
 *
 * @param limiter  The limiter
 * @param active   The number of requests in progress
 * @param maximum  The maximum number of requests that have ever been active
 **/
void getLimiterValuesAtomically(Limiter  *limiter,
                                uint32_t *active,
                                uint32_t *maximum);

/**
 * Initialize a Limiter
 *
 * @param limiter  The limiter
 * @param limit    The limit to the number of active resources
 **/
void initializeLimiter(Limiter *limiter, uint32_t limit);

/**
 * Release resources, making them available for other uses
 *
 * @param limiter  The limiter
 * @param count    The number of resources to release
 **/
void limiterReleaseMany(Limiter *limiter, uint32_t count);

/**
 * Release one resource, making it available for another use
 *
 * @param limiter  The limiter
 **/
static inline void limiterRelease(Limiter *limiter)
{
  limiterReleaseMany(limiter, 1);
}

/**
 * Wait until there are no active resources
 *
 * @param limiter  The limiter
 **/
void limiterWaitForIdle(Limiter *limiter);

/**
 * Prepare to start using one resource, waiting if there are too many
 * resources already in use.  After returning from this routine, the caller
 * may use resource, and must call limiterRelease after freeing those
 * resources.
 *
 * @param limiter  The limiter
 **/
void limiterWaitForOneFree(Limiter *limiter);

#endif /* LIMITER_H */
