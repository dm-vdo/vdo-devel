/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef COMPLETION_UTILS_H
#define COMPLETION_UTILS_H

#include "completion.h"

/*
 *  These functions are analogous to the same functions without the "wrapped"
 *  adjective from asyncLayer.h.  The purpose of the wrapped versions is to
 *  permit this interface to work when the completions used would be
 *  re-used and therefore completed multiple times.
 */

/**
 * Indirectly perform an action using a completion by wrapping it in a
 * temporary completion and performing a meta-action on that.
 *
 * This function is equivalent of calling launchWrappedAction and then
 * immediately calling awaitWrappedCompletion.
 *
 * @param action        the action to enqueue
 * @param completion    the completion to use when the action is complete
 *
 * @return VDO_SUCCESS of an error code
 **/
int performWrappedAction(vdo_action            *action,
                         struct vdo_completion *completion);

/**
 * Launch an arbitrary wrapped action but do not wait for it.
 *
 * @param action        the action to enqueue
 * @param completion    the completion to use when the action is complete
 * @param wrapperPtr    the new wrapper to wait on
 **/
void launchWrappedAction(vdo_action             *action,
                         struct vdo_completion  *completion,
                         struct vdo_completion **wrapperPtr);

/**
 * Wait for an action previously launched by launchWrappedAction()
 *
 * @param wrapper       the wrapped completion provided by
 *                        launchWrappedAction()
 *
 * @return the completion result
 **/
int awaitWrappedCompletion(struct vdo_completion *wrapper);

/**
 * Remove any completion wrapping on a completion.
 *
 * @param completion    a completion which may have been wrapped
 **/
void removeCompletionWrapping(struct vdo_completion *completion);

#endif // COMPLETION_UTILS_H
