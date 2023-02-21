// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "completion.h"

#include <linux/kernel.h>
#ifndef __KERNEL__
#include <string.h>
#include <stdio.h>
#endif /* __KERNEL__ */

#include "logger.h"
#include "permassert.h"

#if defined(INTERNAL) || defined(VDO_INTERNAL)
#include "data-vio.h"
#endif /* INTERNAL or VDO_INTERNAL */
#include "status-codes.h"
#include "thread-config.h"
#include "types.h"
#include "vio.h"
#include "vdo.h"

/**
 * vdo_initialize_completion() - Initialize a completion to a clean state, for reused completions.
 * @completion: The completion to initialize.
 * @vdo: The VDO instance.
 * @type: The type of the completion.
 */
void vdo_initialize_completion(struct vdo_completion *completion,
			       struct vdo *vdo,
			       enum vdo_completion_type type)
{
	memset(completion, 0, sizeof(*completion));
	completion->vdo = vdo;
	completion->type = type;
	vdo_reset_completion(completion);
}

/**
 * vdo_reset_completion() - Reset a completion to a clean state, while keeping the type, vdo and
 *                          parent information.
 * @completion: The completion to reset.
 */
void vdo_reset_completion(struct vdo_completion *completion)
{
	completion->result = VDO_SUCCESS;
	completion->complete = false;
}

/**
 * assert_incomplete() - Assert that a completion is not complete.
 * @completion: The completion to check.
 */
static inline void assert_incomplete(struct vdo_completion *completion)
{
	ASSERT_LOG_ONLY(!completion->complete, "completion is not complete");
}

/**
 * vdo_set_completion_result() - Set the result of a completion.
 * @completion: The completion whose result is to be set.
 * @result: The result to set.
 *
 * Older errors will not be masked.
 */
void vdo_set_completion_result(struct vdo_completion *completion, int result)
{
	assert_incomplete(completion);
	if (completion->result == VDO_SUCCESS)
		completion->result = result;
}

/**
 * vdo_invoke_completion_callback_with_priority() - Invoke the callback of a completion.
 * @completion: The completion whose callback is to be invoked.
 * @priority: The priority at which to enqueue the completion.
 *
 * If called on the correct thread (i.e. the one specified in the completion's callback_thread_id
 * field), the completion will be run immediately. Otherwise, the completion will be enqueued on
 * the correct callback thread.
 */
void vdo_invoke_completion_callback_with_priority(struct vdo_completion *completion,
						  enum vdo_completion_priority priority)
{
	thread_id_t callback_thread = completion->callback_thread_id;

	if (completion->requeue || (callback_thread != vdo_get_callback_thread_id())) {
		vdo_enqueue_completion_with_priority(completion, priority);
		return;
	}

	vdo_run_completion_callback(completion);
}

/**
 * vdo_continue_completion() - Continue processing a completion.
 * @completion: The completion to continue.
 * @result: The current result (will not mask older errors).
 *
 * Continue processing a completion by setting the current result and calling
 * vdo_invoke_completion_callback().
 */
void vdo_continue_completion(struct vdo_completion *completion, int result)
{
	vdo_set_completion_result(completion, result);
	vdo_invoke_completion_callback(completion);
}

/**
 * vdo_complete_completion() - Complete a completion.
 *
 * @completion: The completion to complete.
 */
void vdo_complete_completion(struct vdo_completion *completion)
{
	assert_incomplete(completion);
	completion->complete = true;
	if (completion->callback != NULL)
		vdo_invoke_completion_callback(completion);
}

/**
 * vdo_finish_completion_parent_callback() - A callback to finish the parent of a completion.
 * @completion: The completion which has finished and whose parent should be finished.
 */
void vdo_finish_completion_parent_callback(struct vdo_completion *completion)
{
	vdo_finish_completion((struct vdo_completion *) completion->parent, completion->result);
}

/**
 * vdo_preserve_completion_error_and_continue() - Error handler.
 * @completion: The completion which failed.
 *
 * Error handler which preserves an error in the parent (if there is one), and then resets the
 * failing completion and calls its non-error callback.
 */
void vdo_preserve_completion_error_and_continue(struct vdo_completion *completion)
{
	if (completion->parent != NULL)
		vdo_set_completion_result(completion->parent, completion->result);

	vdo_reset_completion(completion);
	vdo_invoke_completion_callback(completion);
}

/**
 * vdo_noop_completion_callback() - A callback which does nothing.
 * @completion: The completion being called back.
 *
 * This callback is intended to be set as an error handler in the case where an error should do
 * nothing.
 */
void
vdo_noop_completion_callback(struct vdo_completion *completion __always_unused)
{
}

/**
 * vdo_enqueue_completion_with_priority() - Enqueue a completion.
 * @completion: The completion to be enqueued.
 * @priority: The priority at which the work should be done.
 *
 * A function to enqueue a vdo_completion to run on the thread specified by its callback_thread_id
 * field at the specified priority.
 */
void vdo_enqueue_completion_with_priority(struct vdo_completion *completion,
					  enum vdo_completion_priority priority)
{
	struct vdo *vdo = completion->vdo;
	thread_id_t thread_id = completion->callback_thread_id;

	if (ASSERT(thread_id < vdo->thread_config->thread_count,
		   "thread_id %u (completion type %d) is less than thread count %u",
		   thread_id,
		   completion->type,
		   vdo->thread_config->thread_count) != UDS_SUCCESS)
		BUG();

#if defined(INTERNAL) || defined(VDO_INTERNAL)
	if ((completion->type == VIO_COMPLETION) && is_data_vio(as_vio(completion)))
		ASSERT_LOG_ONLY(((completion->error_handler != NULL) ||
				 (as_data_vio(completion)->last_async_operation ==
				  VIO_ASYNC_OP_CLEANUP)),
				"active data_vio has error handler");

#endif /* INTERNAL or VDO_INTERNAL */
	completion->requeue = false;
	completion->priority = priority;
	completion->my_queue = NULL;
	enqueue_work_queue(vdo->threads[thread_id].queue, completion);
}

