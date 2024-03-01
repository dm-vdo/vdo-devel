/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "funnel-workqueue.h"

#include <linux/atomic.h>

#include "event-count.h"
#include "funnel-queue.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"
#include "syscalls.h"
#include "thread-utils.h"

#include "completion.h"
#include "status-codes.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

struct vdo_work_queue {
  char                              *name;
  char                              *threadName;
  volatile bool                      running;
  struct event_count                *wakeEvent;
  struct thread                     *thread;
  const struct vdo_work_queue_type  *type;
  void                             **context;
  struct vdo_thread                 *vdo_thread;
  struct funnel_queue               *queues[];
};

/**
 * Extract the next item from the appropriate queue.
 *
 * @param queue  the work queue
 *
 * @return the funnel queue entry or NULL if shutdown has occurred
 *         @note as currently implemented, the primary queue can starve out
 *         the secondary queue
 **/
static struct funnel_queue_entry *getNextItem(struct vdo_work_queue *queue)
{
  struct funnel_queue_entry *entry = NULL;
  for (int level = queue->type->max_priority;
       (entry == NULL) && (level >= 0);
       level--) {
    entry = vdo_funnel_queue_poll(queue->queues[level]);
  }

  return entry;
}

/**
 * Attempt to dequeue an entry from the queue.  This function should only be
 * used by a single thread when startProcessingWorkQueue() is not being used, or
 * after it has been stopped.
 *
 * <p>This function blocks if no work is available.
 *
 * @param queue  the queue
 *
 * @return the work to perform, or NULL if the queue is drained, the run
 *         state is false, and the reservation count is zero.
 **/
static struct funnel_queue_entry *
dequeueWorkQueueEntry(struct vdo_work_queue *queue)
{
  for (;;) {
    // fast path
    struct funnel_queue_entry *entry = getNextItem(queue);
    if (entry != NULL) {
      return entry;
    }

    // prepare to wait
    event_token_t waitToken = event_count_prepare(queue->wakeEvent);

    // retest
    entry = getNextItem(queue);
    if (entry != NULL) {
      event_count_cancel(queue->wakeEvent, waitToken);
      return entry;
    }

    // are we done?
    if (!READ_ONCE(queue->running)) {
      event_count_cancel(queue->wakeEvent, waitToken);
      break;
    }

    // wait
    event_count_wait(queue->wakeEvent, waitToken, NULL);
  }

  return NULL;
}

/**
 * Thread processing function for a work queue. Conforms to signature required
 * by pthread_start.
 *
 * @param arg   the context (in this case, the queue itself)
 **/
static void queueRunner(void *arg)
{
  struct vdo_work_queue *queue = arg;

  vdo_log_debug("started %s", queue->threadName);

  if (queue->type->start != NULL) {
    queue->type->start(*(queue->context));
  }

  struct funnel_queue_entry *entry;
  while ((entry = dequeueWorkQueueEntry(queue)) != NULL) {
    struct vdo_completion *completion = container_of(entry,
                                                     struct vdo_completion,
                                                     work_queue_entry_link);
    if (VDO_ASSERT(completion->my_queue == queue,
                   "completion %px from queue %s marked as being in this queue (%px)",
                   (void *) completion,
                   queue->name,
                   (void *) completion->my_queue) == VDO_SUCCESS) {
      completion->my_queue = NULL;
    }

    enum vdo_completion_priority priority = completion->priority;
    vdo_run_completion(completion);
    runFinishedHook(priority);
  }

  if (queue->type->finish != NULL) {
    queue->type->finish(*(queue->context));
  }

  vdo_log_debug("finished %s", queue->threadName);
}

/*****************************************************************************/
int vdo_make_work_queue(const char *thread_name_prefix,
                        const char *name,
                        struct vdo_thread *owner,
                        const struct vdo_work_queue_type *type,
                        unsigned int thread_count  __attribute__((unused)),
                        void *privates[],
                        struct vdo_work_queue **queue_ptr)
{
  struct vdo_work_queue *queue;
  uint8_t priorityLevels = type->max_priority + 1;

  STATIC_ASSERT((int) NO_HOOK_FLAG > (int) VDO_WORK_Q_DEFAULT_PRIORITY);
  STATIC_ASSERT((int) WORK_FLAG    > (int) VDO_WORK_Q_DEFAULT_PRIORITY);
  VDO_ASSERT_SUCCESS(vdo_allocate_extended(struct vdo_work_queue,
                                           priorityLevels,
                                           struct funnel_queue *,
                                           __func__,
                                           &queue));
  VDO_ASSERT_SUCCESS(vdo_duplicate_string(name,
                                          "work queue name",
                                          &queue->name));
  VDO_ASSERT_SUCCESS(vdo_alloc_sprintf("work queue thread name",
                                       &queue->threadName,
                                       "%s%s",
                                       thread_name_prefix,
                                       queue->name));
  VDO_ASSERT_SUCCESS(make_event_count(&queue->wakeEvent));
  for (int i = 0; i < priorityLevels; i++) {
    VDO_ASSERT_SUCCESS(vdo_make_funnel_queue(&queue->queues[i]));
  }

  queue->vdo_thread = owner;
  queue->type       = type;
  queue->context    = privates;
  WRITE_ONCE(queue->running, true);

  VDO_ASSERT_SUCCESS(vdo_create_thread(queueRunner,
                                       queue,
                                       queue->threadName,
                                       &queue->thread));

  *queue_ptr = queue;
  return VDO_SUCCESS;
}

/*****************************************************************************/
void vdo_free_work_queue(struct vdo_work_queue *queue)
{
  if (queue == NULL) {
    return;
  }

  vdo_finish_work_queue(queue);
  for (enum vdo_completion_priority i = 0;
       i <= queue->type->max_priority;
       i++) {
    vdo_free_funnel_queue(queue->queues[i]);
  }

  free_event_count(queue->wakeEvent);
  vdo_free(queue->threadName);
  vdo_free(queue->name);
  vdo_free(queue);
}

/*****************************************************************************/
void vdo_enqueue_work_queue(struct vdo_work_queue *queue,
                            struct vdo_completion *completion)
{
  if (!runEnqueueHook(completion)) {
    return;
  }

  enum vdo_completion_priority priority = completion->priority & PRIORITY_MASK;
  if (priority == VDO_WORK_Q_DEFAULT_PRIORITY) {
    priority = queue->type->default_priority;
  }

  CU_ASSERT(priority <= queue->type->max_priority);
  completion->my_queue = queue;
  vdo_funnel_queue_put(queue->queues[priority],
                       &completion->work_queue_entry_link);
  event_count_broadcast(queue->wakeEvent);
}

/*****************************************************************************/
void vdo_finish_work_queue(struct vdo_work_queue *queue)
{
  if (queue == NULL) {
    return;
  }

  if (READ_ONCE(queue->running)) {
    WRITE_ONCE(queue->running, false);
    event_count_broadcast(queue->wakeEvent);
    vdo_join_threads(queue->thread);
  }
}

/**********************************************************************/
void *vdo_get_work_queue_private_data(void)
{
  thread_id_t thread = vdo_get_callback_thread_id();
  return ((thread == VDO_INVALID_THREAD_ID)
          ? NULL
          : *(vdo->threads[thread].queue->context));
}

/*
 * Implements vdo_filter_fn.
 */
static bool all_vdos_match(struct vdo *vdo __attribute__((unused)),
                           const void *context __attribute__((unused)))
{
  return true;
}

/**********************************************************************/
struct vdo_work_queue *vdo_get_current_work_queue(void)
{
  if (vdo == NULL) {
    vdo = vdo_find_matching(all_vdos_match, NULL);
    if (vdo == NULL) {
      return NULL;
    }
  }

  pthread_t currentThread = pthread_self();
  for (thread_id_t id = 0; id < vdo->thread_config.thread_count; id++) {
    struct vdo_work_queue *queue = vdo->threads[id].queue;
    if ((queue != NULL)
        && (pthread_equal(currentThread, queue->thread->thread))) {
      return queue;
    }
  }

  return NULL;
}

/**********************************************************************/
struct vdo_thread *vdo_get_work_queue_owner(struct vdo_work_queue *queue)
{
  return queue->vdo_thread;
}

/**********************************************************************/
bool vdo_work_queue_type_is(struct vdo_work_queue *queue,
                            const struct vdo_work_queue_type *type)
{
  return (queue->type == type);
}

/**********************************************************************/
void vdo_dump_completion_to_buffer(struct vdo_completion *completion,
                                   char *buffer,
                                   size_t length)
{
  size_t current_length = snprintf(buffer,
                                   length,
                                   "%.*s/",
                                   TASK_COMM_LEN,
                                   (completion->my_queue == NULL ?
                                    "-" :
                                    completion->my_queue->name));
  if (current_length < length) {
    if (completion->callback == NULL) {
      strncpy(buffer + current_length, "-", length - current_length);
    } else {
      snprintf(buffer + current_length,
               length - current_length,
               "%lu",
               (uintptr_t) completion->callback);
    }
  }
}

void vdo_dump_work_queue(struct vdo_work_queue *queue)
{
  vdo_log_info("workQ %s %s",
               queue->name,
               (READ_ONCE(queue->running) ? "running" : "idle"));
}
