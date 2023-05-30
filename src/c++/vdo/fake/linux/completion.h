/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 *
 */

#ifndef LINUX_COMPLETION_H
#define LINUX_COMPLETION_H

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

#endif // LINUX_COMPLETION_H
