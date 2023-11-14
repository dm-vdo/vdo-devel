/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "callbackWrappingUtils.h"

#include "memory-alloc.h"
#include "uds-threads.h"

#include "completion.h"
#include "int-map.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  vdo_action_fn callback;
  vdo_action_fn errorHandler;
} SavedActions;

static struct int_map          *wrapMap    = NULL;
static struct int_map          *enqueueMap = NULL;
static struct mutex             mutex;

/**
 * Implements TearDownAction
 **/
static void tearDown(void)
{
  vdo_free_int_map(uds_forget(wrapMap));
  vdo_free_int_map(uds_forget(enqueueMap));
  uds_destroy_mutex(&mutex);
}

/**********************************************************************/
void initializeCallbackWrapping(void)
{
  uds_initialize_mutex(&mutex, true);
  VDO_ASSERT_SUCCESS(vdo_make_int_map(0, 0, &wrapMap));
  VDO_ASSERT_SUCCESS(vdo_make_int_map(0, 0, &enqueueMap));
  registerTearDownAction(tearDown);
}

/**********************************************************************/
static void wrapCompletion(struct vdo_completion *completion,
                           vdo_action_fn          callback,
                           vdo_action_fn          errorHandler)
{
  CU_ASSERT_PTR_NOT_NULL(completion->callback);

  SavedActions *actions;
  VDO_ASSERT_SUCCESS(uds_allocate(1, SavedActions, __func__, &actions));
  *actions = (SavedActions) {
    .callback     = completion->callback,
    .errorHandler = completion->error_handler,
  };

  SavedActions *old;
  uds_lock_mutex(&mutex);
  VDO_ASSERT_SUCCESS(vdo_int_map_put(wrapMap,
                                     (uintptr_t) completion,
                                     actions,
                                     false,
                                     (void **) &old));
  uds_unlock_mutex(&mutex);
  CU_ASSERT_PTR_NULL(old);

  completion->callback      = callback;
  completion->error_handler = errorHandler;
}

/**********************************************************************/
void
wrapCompletionCallbackAndErrorHandler(struct vdo_completion *completion,
                                      vdo_action_fn          callback,
                                      vdo_action_fn          errorHandler)
{
  wrapCompletion(completion, callback, errorHandler);
}

/**
 * Run the saved callback (from a callback wrapper).
 *
 * @param completion  The completion
 *
 * @return whether or not the completion requeued
 **/
static bool runSaved(struct vdo_completion *completion)
{
  bool requeued = false;
  bool *old = NULL;

  uds_lock_mutex(&mutex);
  SavedActions *actions = vdo_int_map_remove(wrapMap, (uintptr_t) completion);
  VDO_ASSERT_SUCCESS(vdo_int_map_put(enqueueMap,
                                     (uintptr_t) completion,
                                     &requeued,
                                     false,
                                     (void **) &old));
  uds_unlock_mutex(&mutex);

  CU_ASSERT_PTR_NOT_NULL(actions);
  CU_ASSERT_PTR_NULL(old);

  completion->callback      = actions->callback;
  completion->error_handler = actions->errorHandler;
  uds_free(actions);
  vdo_run_completion(completion);

  if (requeued) {
    return true;
  }

  uds_lock_mutex(&mutex);
  vdo_int_map_remove(enqueueMap, (uintptr_t) completion);
  uds_unlock_mutex(&mutex);

  return false;
}

/**********************************************************************/
bool runSavedCallback(struct vdo_completion *completion)
{
  return runSaved(completion);
}

/**********************************************************************/
void runSavedCallbackAssertRequeue(struct vdo_completion *completion)
{
  CU_ASSERT(runSavedCallback(completion));
}

/**********************************************************************/
void runSavedCallbackAssertNoRequeue(struct vdo_completion *completion)
{
  CU_ASSERT_FALSE(runSavedCallback(completion));
}

/**********************************************************************/
void notifyEnqueue(struct vdo_completion *completion)
{
  uds_lock_mutex(&mutex);
  bool *requeued = vdo_int_map_remove(enqueueMap, (uintptr_t) completion);
  if (requeued != NULL) {
    *requeued = true;
  }
  uds_unlock_mutex(&mutex);
}

