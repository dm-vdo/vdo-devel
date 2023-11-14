/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef CALLBACK_WRAPPING_UTILS_H
#define CALLBACK_WRAPPING_UTILS_H

#include "completion.h"
#include "types.h"

/**
 * Initialized the callback wrapping infrastructure. This method should only be
 * called from initializeVDOTestBase().
 **/
void initializeCallbackWrapping(void);

/**
 * Individually wrap the callback and error handler of a completion.
 *
 * @param completion    The completion
 * @param callback      The wrapper callback
 * @param errorHandler  The wrapper error handler
 **/
void
wrapCompletionCallbackAndErrorHandler(struct vdo_completion *completion,
                                      vdo_action_fn          callback,
                                      vdo_action_fn          errorHandler);

/**
 * Wrap the callback of a completion. The error handler will also be wrapped
 * with the same callback.
 *
 * @param completion  The completion
 * @param callback    The wrapper callback
 **/
static inline void wrapCompletionCallback(struct vdo_completion *completion,
                                          vdo_action_fn          callback)
{
  wrapCompletionCallbackAndErrorHandler(completion, callback, callback);
}

/**
 * Run the saved callback (used from a callback wrapper). The completion must
 * not be examined after this function is called, and the callback must not
 * free the completion.
 *
 * @param completion  The completion
 *
 * @return true if the completion was requeued
 **/
bool runSavedCallback(struct vdo_completion *completion);

/**
 * Run the saved callback (used from a callback wrapper), and assert that
 * it has requeued. This may not run a callback which may free the completion.
 *
 * @param completion  The completion
 **/
void runSavedCallbackAssertRequeue(struct vdo_completion *completion);

/**
 * Run the saved callback (used from a callback wrapper), and assert it
 * is not requeued. This may not run a callback which may free the completion.
 *
 * @param completion  The completion
 **/
void runSavedCallbackAssertNoRequeue(struct vdo_completion *completion);

/**
 * Inform the infrastructure that a completion is being enqueued.
 *
 * @param completion  The completion
 **/
void notifyEnqueue(struct vdo_completion *completion);

#endif // CALLBACK_WRAPPING_UTILS_H
