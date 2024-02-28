/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "completionUtils.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"

/**
 * This type of completion is used to wrap a completion which is re-used
 * so that the asyncLayer performAction (or launchAction and awaitCompletion)
 * function(s) work correctly.
 **/
typedef struct wrappingCompletion {
  struct vdo_completion  completion;    ///< common completion header
  struct vdo_completion *original;      ///< original completion
  vdo_action_fn          action;        ///< action to perform on original
  vdo_action_fn          savedCallback; ///< saved completion callback
  struct vdo_completion *savedParent;   ///< saved completion parent
} WrappingCompletion;

/**********************************************************************/
static inline WrappingCompletion *
asWrappingCompletion(struct vdo_completion *completion)
{
  vdo_assert_completion_type(completion, VDO_WRAPPING_COMPLETION);
  return container_of(completion, WrappingCompletion, completion);
}

/**********************************************************************/
static int makeWrappingCompletion(vdo_action_fn           action,
                                  struct vdo_completion  *completion,
                                  struct vdo_completion **wrappingCompletion)
{
  WrappingCompletion *wc;
  int result = vdo_allocate(1, WrappingCompletion, "wrapping completion", &wc);
  if (result != VDO_SUCCESS) {
    return result;
  }

  *wc = (WrappingCompletion) {
    .original      = completion,
    .action        = action,
    .savedParent   = completion->parent,
    .savedCallback = completion->callback,
  };
  vdo_initialize_completion(&wc->completion, completion->vdo,
                            VDO_WRAPPING_COMPLETION);

  *wrappingCompletion = &wc->completion;
  return VDO_SUCCESS;
}

/**********************************************************************/
static void freeWrappingCompletion(WrappingCompletion *wc)
{
  if (wc != NULL) {
    wc->original->parent   = wc->savedParent;
    wc->original->callback = wc->savedCallback;
    vdo_free(wc);
  }
}

/**********************************************************************/
void removeCompletionWrapping(struct vdo_completion *completion)
{
  if (completion->parent != NULL) {
    freeWrappingCompletion(asWrappingCompletion(completion->parent));
  }
}

/**
 * This is the proxy callback that the original action actually calls.
 **/
static void finishWrapping(struct vdo_completion *completion)
{
  WrappingCompletion *wc = asWrappingCompletion(completion->parent);
  if (wc->savedCallback != NULL) {
    wc->original->callback = wc->savedCallback;
    wc->original->parent   = wc->savedParent;
    wc->savedCallback(wc->original);
  }

  vdo_fail_completion(&wc->completion, wc->original->result);
}

/**
 * This is the proxy action triggered by launchWrappedAction.
 *
 * It is responsible for saving the callback state of the original completion
 * and substituting a redirection to the wrapper proxy callback
 * finishWrapping() before performing the original intended action.
 **/
static void doWrappedAction(struct vdo_completion *completion)
{
  WrappingCompletion *wc = asWrappingCompletion(completion);

  wc->savedParent         = wc->original->parent;
  wc->savedCallback       = wc->original->callback;
  wc->original->parent    = &wc->completion;
  wc->original->callback  = finishWrapping;
  wc->action(wc->original);
}

/**********************************************************************/
void launchWrappedAction(vdo_action_fn           action,
                         struct vdo_completion  *completion,
                         struct vdo_completion **wrapperPtr)
{
  struct vdo_completion *wrapper;
  VDO_ASSERT_SUCCESS(makeWrappingCompletion(action, completion, &wrapper));
  *wrapperPtr = wrapper;
  launchAction(doWrappedAction, wrapper);
}

/**********************************************************************/
int awaitWrappedCompletion(struct vdo_completion *wrapper)
{
  WrappingCompletion *wc = asWrappingCompletion(wrapper);
  int result = awaitCompletion(&wc->completion);
  freeWrappingCompletion(wc);
  return result;
}

/**********************************************************************/
int performWrappedAction(vdo_action_fn          action,
                         struct vdo_completion *completion)
{
  struct vdo_completion *wrapper;
  launchWrappedAction(action, completion, &wrapper);
  return awaitWrappedCompletion(wrapper);
}

/**
 * Finish a completion's parent with the result of the completion.
 *
 * Implements vdo_action_fn.
 **/
void finishParentCallback(struct vdo_completion *completion)
{
  vdo_fail_completion(completion->parent, completion->result);
}
