/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef ASYNC_LAYER_H
#define ASYNC_LAYER_H

#include "completion.h"
#include "types.h"
#include "workQueue.h"

#include "physicalLayer.h"

#include "asyncVIO.h"
#include "testParameters.h"

typedef bool CompletionHook(struct vdo_completion *completion);
typedef void FinishedHook(void);

/**
 * A hook which will be called from submit_bio_noacct().
 *
 * @param bio  The bio being submitted
 *
 * @return true if the bio should be submitted
 **/
typedef bool BIOSubmitHook(struct bio *bio);

enum {
  NO_HOOK_FLAG  = 1 << 3,
  WORK_FLAG     = 1 << 5,
  PRIORITY_MASK = ~(NO_HOOK_FLAG | WORK_FLAG),
};


/**
 * Initialize the layer in vdoTestBase.
 *
 * @param syncLayer  The synchronous layer to wrap
 **/
void initializeAsyncLayer(PhysicalLayer *syncLayer);

/**
 * Free the AsyncLayer.
 **/
void destroyAsyncLayer(void);

/**
 * Start the layer and optionally load the VDO on it.
 *
 * @param configuration  The test configuration
 * @param loadVDO        If <code>true</code> load the VDO
 **/
void startAsyncLayer(TestConfiguration configuration, bool loadVDO);

/**
 * Stop the asynchronous threads.
 **/
void stopAsyncLayer(void);

/**
 * Set the read-only state of the layer.
 *
 * @param readOnly  <code>true</code> to cause the layer to return EROFS on
 *                  any write request
 **/
void setAsyncLayerReadOnly(bool readOnly);

/**
 * Launch an arbitrary asynchronous VDO operation but do not wait for the
 * result.
 *
 * Note that this method will set the completion callback to its own
 * private callback.
 *
 * @param action      The action to enqueue
 * @param completion  The completion to use when the action is complete
 *
 * @note Callers must call awaitCompletion() on the completion parameter
 *      to await the result.
 **/
void launchAction(vdo_action *action, struct vdo_completion *completion);

/**
 * Low-level operation to wait for an operation started by launchAction()
 * to complete. A completion may only be waited on once per launch.
 *
 * @param completion  The completion to wait for
 *
 * @return The completion result
 **/
int awaitCompletion(struct vdo_completion *completion);

/**
 * Perform an arbitrary asynchronous VDO operation and wait for the result.
 *
 * The request is enqueued and this function waits for the request to
 * execute, using a private callback in the provided completion.
 * (This completion must otherwise match the requested action.)
 *
 * @param action      The action to enqueue
 * @param completion  The completion to use when the action is complete
 *
 * @return VDO_SUCCESS or an error code
 **/
int performAction(vdo_action *action, struct vdo_completion *completion);

/**
 * Enqueue a completion on a vdo thread skipping the callback hook.
 *
 * @param vio  The VIO to enqueue
 **/
void reallyEnqueueCompletion(struct vdo_completion *completion);

/**
 * Enqueue a vio on a vdo thread skipping the callback hook.
 *
 * @param vio The VIO to enqueue
 **/
static inline void reallyEnqueueVIO(struct vio *vio)
{
  reallyEnqueueCompletion(vio_as_completion(vio));
}

/**
 * Remove a function from the list of completion enqueue hooks.
 *
 * @param function  The function to remove
 **/
void removeCompletionEnqueueHook(CompletionHook *function);

/**
 * Remove all functions from the list of completion enqueue hooks.
 **/
void clearCompletionEnqueueHooks(void);

/**
 * Add a function to the list of completion enqueue hooks.
 *
 * @param function  The function to add
 **/
void addCompletionEnqueueHook(CompletionHook *function);

/**
 * Replace any functions on the list of completion enqueue hooks with the
 * specified single function.
 *
 * @param function  The function to become the only one on the list
 **/
void setCompletionEnqueueHook(CompletionHook *function);

/**
 * Register a function as the post-execution callback hook. This function will
 * be called immediately after a completion has been processed, including all
 * callbacks.
 *
 * @param function The function to register, or NULL for default behavior
 **/
void setCallbackFinishedHook(FinishedHook *function);

/**
 * Clear all registered hooks from the layer.
 **/
void clearLayerHooks(void);

/**
 * Set the expectation for the result of starting or stopping the VDO.
 *
 * @param common          The layer whose expectations are to be set
 * @param expectedResult  The expected result
 **/
void setStartStopExpectation(int expectedResult);

/**
 * Run all registered enqueue hooks until one returns false or all return true.
 *
 * @param completion  The completion to be enqueued
 *
 * @return true if the completion should be enqueued
 **/
bool runEnqueueHook(struct vdo_completion *completion);

/**
 * Run the appropriate finished hook for a completion. The priority flags are
 * used to determine which hook to run.
 *
 * @param priority  The priority with which the item was enqueued
 **/
void runFinishedHook(enum vdo_completion_priority priority);

/**
 * Set the bio submission hook.
 *
 * @param function  The function to set
 **/
void setBIOSubmitHook(BIOSubmitHook *function);

/**
 * Clear the bio submission hook.
 **/
static inline void clearBIOSubmitHook(void)
{
  setBIOSubmitHook(NULL);
}

/**
 * Enqueue a bio to be processed below the VDO without checking the bio
 * submission hook.
 *
 * @param bio  The bio to enqueue
 **/
void reallyEnqueueBIO(struct bio *bio);

/**
 * Enqueue a bio to be processed below the VDO if the bio submission hook says
 * we may.
 *
 * @param bio  The bio to enqueue
 **/
void enqueueBIO(struct bio *bio);

/**
 * Check whether we are on the bio processing thread.
 **/
bool onBIOThread(void);

#endif // ASYNC_LAYER_H
