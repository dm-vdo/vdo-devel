/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit tests need access to struct completion which comes from
 * linux/completion.h but is included in vdo via linux/delay.h.
 *
 * $Id$
 */
#ifndef LINUX_DELAY_H
#define LINUX_DELAY_H

#include "uds-threads.h"

/*
 * struct completion - structure used to maintain state for a "completion"
 */
struct completion {
  bool done;
  struct mutex mutex;
  struct cond_var condition;
};

/**
 * Initialize a completion.
 *
 * @param completion  The completion to initialize
 */
void init_completion(struct completion *completion);

/**
 * Re-initialize a completion.
 *
 * @param completion  The completion to re-initialize
 **/
void reinit_completion(struct completion *completion);

/**
 * Block until a completion is done.
 *
 * @param completion  The completion to wait on
 **/
void wait_for_completion(struct completion *completion);

/**
 * Block until a completion is done or a signal is received. In unit tests,
 * just block.
 *
 * @param completion  The completion to wait on
 *
 * @return 0 (success)
 **/
static inline int __must_check
wait_for_completion_interruptible(struct completion *completion)
{
  wait_for_completion(completion);
  return 0;
}

/**
 * Mark a completion as done and signal any waiters.
 *
 * @param completion  The completion to complete
 **/
void complete(struct completion *completion);

/**
 * Sleep safely, even with waitqueue interruptions. For unit tests, doesn't
 * actually sleep.
 *
 * @param usecs  The number of microseconds we won't sleep for
 **/
static inline void fsleep(unsigned int usecs)
{
  return;
}

#endif // LINUX_DELAY_H
