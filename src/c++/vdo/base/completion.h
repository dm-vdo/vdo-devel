/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_COMPLETION_H
#define VDO_COMPLETION_H

#include "permassert.h"

#include "status-codes.h"
#include "types.h"

/**
 * vdo_run_completion_callback() - Actually run the callback.
 *
 * Context: This function must be called from the correct callback thread.
 */
static inline void vdo_run_completion_callback(struct vdo_completion *completion)
{
	if ((completion->result != VDO_SUCCESS) && (completion->error_handler != NULL)) {
		completion->error_handler(completion);
		return;
	}

	completion->callback(completion);
}

void vdo_set_completion_result(struct vdo_completion *completion, int result);

void vdo_initialize_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type);

void vdo_reset_completion(struct vdo_completion *completion);

void vdo_invoke_completion_callback_with_priority(struct vdo_completion *completion,
						  enum vdo_completion_priority priority);

/**
 * vdo_invoke_completion_callback() - Invoke the callback of a completion.
 *
 * If called on the correct thread (i.e. the one specified in the completion's callback_thread_id
 * field), the completion will be run immediately. Otherwise, the completion will be enqueued on
 * the correct callback thread.
 */
static inline void vdo_invoke_completion_callback(struct vdo_completion *completion)
{
	vdo_invoke_completion_callback_with_priority(completion, VDO_WORK_Q_DEFAULT_PRIORITY);
}

void vdo_continue_completion(struct vdo_completion *completion, int result);

void vdo_complete_completion(struct vdo_completion *completion);

/**
 * vdo_finish_completion() - Finish a completion.
 * @result: The result of the completion (will not mask older errors).
 */
static inline void vdo_finish_completion(struct vdo_completion *completion, int result)
{
	vdo_set_completion_result(completion, result);
	vdo_complete_completion(completion);
}

void vdo_preserve_completion_error_and_continue(struct vdo_completion *completion);

/**
 * vdo_assert_completion_type() - Assert that a completion is of the correct type.
 *
 * Return: VDO_SUCCESS or an error
 */
static inline int
vdo_assert_completion_type(enum vdo_completion_type actual, enum vdo_completion_type expected)
{
	return ASSERT(expected == actual, "completion type is %u instead of %u", actual, expected);
}

/** vdo_set_completion_callback() - Set the callback for a completion. */
static inline void vdo_set_completion_callback(struct vdo_completion *completion,
					       vdo_action *callback,
					       thread_id_t callback_thread_id)
{
	completion->callback = callback;
	completion->callback_thread_id = callback_thread_id;
}

/**
 * vdo_launch_completion_callback() - Set the callback for a completion and invoke it immediately.
 */
static inline void vdo_launch_completion_callback(struct vdo_completion *completion,
						  vdo_action *callback,
						  thread_id_t callback_thread_id)
{
	vdo_set_completion_callback(completion, callback, callback_thread_id);
	vdo_invoke_completion_callback(completion);
}

/** vdo_set_completion_callback_with_parent() - Set the callback and parent for a completion. */
static inline void
vdo_set_completion_callback_with_parent(struct vdo_completion *completion,
					vdo_action *callback,
					thread_id_t callback_thread_id,
					void *parent)
{
	vdo_set_completion_callback(completion, callback, callback_thread_id);
	completion->parent = parent;
}

/**
 * vdo_launch_completion_callback_with_parent() - Set the callback and parent for a completion and
 *                                                invoke the callback immediately.
 */
static inline void
vdo_launch_completion_callback_with_parent(struct vdo_completion *completion,
					   vdo_action *callback,
					   thread_id_t callback_thread_id,
					   void *parent)
{
	vdo_set_completion_callback_with_parent(completion, callback, callback_thread_id, parent);
	vdo_invoke_completion_callback(completion);
}

/**
 * vdo_prepare_completion() - Prepare a completion for launch.
 *
 * Resets the completion, and then sets its callback, error handler, callback thread, and parent.
 */
static inline void vdo_prepare_completion(struct vdo_completion *completion,
					  vdo_action *callback,
					  vdo_action *error_handler,
					  thread_id_t callback_thread_id,
					  void *parent)
{
	vdo_reset_completion(completion);
	vdo_set_completion_callback_with_parent(completion, callback, callback_thread_id, parent);
	completion->error_handler = error_handler;
}

/**
 * vdo_prepare_completion_for_requeue() - Prepare a completion for launch ensuring that it will
 *                                        always be requeued.
 *
 * Resets the completion, and then sets its callback, error handler, callback thread, and parent.
 */
static inline void
vdo_prepare_completion_for_requeue(struct vdo_completion *completion,
				   vdo_action *callback,
				   vdo_action *error_handler,
				   thread_id_t callback_thread_id,
				   void *parent)
{
	vdo_prepare_completion(completion, callback, error_handler, callback_thread_id, parent);
	completion->requeue = true;
}

void vdo_enqueue_completion_with_priority(struct vdo_completion *completion,
					  enum vdo_completion_priority priority);

/**
 * vdo_enqueue_completion() - Enqueue a vdo_completion to run on the thread specified by its
 *                            callback_thread_id field at default priority.
 */
static inline void vdo_enqueue_completion(struct vdo_completion *completion)
{
	vdo_enqueue_completion_with_priority(completion, VDO_WORK_Q_DEFAULT_PRIORITY);
}

#endif /* VDO_COMPLETION_H */
