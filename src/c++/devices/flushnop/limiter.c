/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "limiter.h"

#include <linux/sched.h>

/**********************************************************************/
void getLimiterValuesAtomically(Limiter  *limiter,
                                uint32_t *active,
                                uint32_t *maximum)
{
  spin_lock(&limiter->lock);
  *active  = limiter->active;
  *maximum = limiter->maximum;
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
void initializeLimiter(Limiter *limiter, uint32_t limit)
{
  limiter->active  = 0;
  limiter->limit   = limit;
  limiter->maximum = 0;
  init_waitqueue_head(&limiter->waiterQueue);
  spin_lock_init(&limiter->lock);
}

/**********************************************************************/
void limiterReleaseMany(Limiter *limiter, uint32_t count)
{
  spin_lock(&limiter->lock);
  limiter->active -= count;
  spin_unlock(&limiter->lock);
  if (waitqueue_active(&limiter->waiterQueue)) {
    wake_up_nr(&limiter->waiterQueue, count);
  }
}

/**********************************************************************/
void limiterWaitForIdle(Limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (limiter->active > 0) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiterQueue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiterQueue, &wait);
  };
  spin_unlock(&limiter->lock);
}

/**********************************************************************/
void limiterWaitForOneFree(Limiter *limiter)
{
  spin_lock(&limiter->lock);
  while (limiter->active >= limiter->limit) {
    DEFINE_WAIT(wait);
    prepare_to_wait_exclusive(&limiter->waiterQueue, &wait,
                              TASK_UNINTERRUPTIBLE);
    spin_unlock(&limiter->lock);
    io_schedule();
    spin_lock(&limiter->lock);
    finish_wait(&limiter->waiterQueue, &wait);
  };
  limiter->active += 1;
  if (limiter->active > limiter->maximum) {
    limiter->maximum = limiter->active;
  }
  spin_unlock(&limiter->lock);
}
