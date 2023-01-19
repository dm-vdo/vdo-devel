 /*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "asyncLayer.h"

#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/list.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "syscalls.h"
#include "uds.h"
#include "uds-threads.h"

#include "dedupe.h"
#include "device-config.h"
#include "instance-number.h"
#include "io-submitter.h"
#include "lz4.h"
#include "status-codes.h"
#include "thread-config.h"
#include "vdo.h"
#include "volume-geometry.h"
#include "work-queue.h"

#include "physicalLayer.h"

#include "callbackWrappingUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "testPrototypes.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef enum {
  LAYER_INITIALIZED,
  QUEUES_STARTED,
  TABLE_LOADED,
  VDO_LOADED,
} AsyncLayerState;

enum {
  // This should be larger than we should ever need.
  MAX_HOOK_COUNT = 16,
};

typedef struct {
  struct list_head  listEntry;
  CompletionHook   *function;
} CompletionHookEntry;

typedef struct {
  PhysicalLayer             common;
  AsyncLayerState           state;
  PhysicalLayer            *syncLayer;
  pid_t                     testThreadID;
  bool                      readOnly;
  bool                      indexOpen;
  atomic64_t                requestCount;
  struct list_head          completionEnqueueHooks;
  struct int_map           *completionEnqueueHooksMap;
  CompletionHook           *completionEnqueueHooksCache[MAX_HOOK_COUNT];
  uint8_t                   completionEnqueueHookCount;
  bool                      completionEnqueueHooksCacheValid;
  FinishedHook             *callbackFinishedHook;
  struct bio_list           bios;
  struct thread            *bioThread;
  pthread_t                 bioThreadID;
  bool                      running;
  BIOSubmitHook            *bioHook;
  struct mutex              mutex;
  struct cond_var           condition;
  bool                      noFlushSuspend;
  int                       startStopExpectation;
} AsyncLayer;

/**
 * Convert the vdoTestBase's layer to an AsyncLayer
 *
 * @return The layer as an AsyncLayer
 **/
__attribute__((warn_unused_result))
static AsyncLayer *asAsyncLayer(void)
{
  STATIC_ASSERT(offsetof(AsyncLayer, common) == 0);
  return (AsyncLayer *) layer;
}

/**
 * Assert that we are running on the test thread, i.e. the thread which
 * constructed the layer.
 **/
static void assertOnTestThread(void)
{
  CU_ASSERT_EQUAL(asAsyncLayer()->testThreadID, uds_get_thread_id());
}

/**
 * Implements PhysicalLayer block_count_getter
 **/
static block_count_t
getBlockCount(PhysicalLayer *common __attribute__((unused)))
{
  PhysicalLayer *syncLayer = asAsyncLayer()->syncLayer;
  return syncLayer->getBlockCount(syncLayer);
}

/**
 * Implements buffer_allocator.
 **/
static int allocateIOBuffer(PhysicalLayer  *common __attribute__((unused)),
                            size_t          bytes,
                            const char     *why,
                            char          **bufferPtr)
{
  return UDS_ALLOCATE(bytes, char, why, bufferPtr);
}

/**
 * Implements PhysicalLayer extent_reader
 **/
static int asyncReader(PhysicalLayer           *common __attribute__((unused)),
                       physical_block_number_t  startBlock,
                       size_t                   blockCount,
                       char                    *buffer)
{
  PhysicalLayer *syncLayer = asAsyncLayer()->syncLayer;
  return syncLayer->reader(syncLayer, startBlock, blockCount, buffer);
}

/**
 * Implements PhysicalLayer extent_writer
 **/
static int asyncWriter(PhysicalLayer           *common __attribute__((unused)),
                       physical_block_number_t  startBlock,
                       size_t                   blockCount,
                       char                    *buffer)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  if (READ_ONCE(asyncLayer->readOnly)) {
    return -EROFS;
  }

  PhysicalLayer *syncLayer = asyncLayer->syncLayer;
  return syncLayer->writer(syncLayer, startBlock, blockCount, buffer);
}

/**********************************************************************/
static void flushSyncLayer(void)
{
  // XXX: if we ever have a sync layer which isn't a RAMLayer but supports
  //      flushes, this will break.
  flushRAMLayer(asAsyncLayer()->syncLayer);
}

/**********************************************************************/
void destroyAsyncLayer(void)
{
  if (layer == NULL) {
    return;
  }

  AsyncLayer *asyncLayer = asAsyncLayer();
  switch (asyncLayer->state) {
  case VDO_LOADED:
  case TABLE_LOADED:
  case QUEUES_STARTED:
    stopAsyncLayer();
    // fall through

  case LAYER_INITIALIZED:
    uds_destroy_cond(&asyncLayer->condition);
    uds_destroy_mutex(&asyncLayer->mutex);
    free_int_map(UDS_FORGET(asyncLayer->completionEnqueueHooksMap));
    break;

  default:
    CU_FAIL("Unknown Async Layer state: %d", asyncLayer->state);
  }

  UDS_FREE(UDS_FORGET(layer));
}

/**
 * The default bio submit hook, which always returns true.
 *
 * Implements BIOSubmitHook.
 **/
static bool defaultBIOSubmitHook(struct bio *bio __attribute__((unused)))
{
  return true;
}

/**********************************************************************/
void initializeAsyncLayer(PhysicalLayer *syncLayer)
{
  AsyncLayer *asyncLayer;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, AsyncLayer, __func__, &asyncLayer));
  VDO_ASSERT_SUCCESS(make_int_map(0,
                                  0,
                                  &asyncLayer->completionEnqueueHooksMap));
  VDO_ASSERT_SUCCESS(uds_init_mutex(&asyncLayer->mutex));
  VDO_ASSERT_SUCCESS(uds_init_cond(&asyncLayer->condition));
  INIT_LIST_HEAD(&asyncLayer->completionEnqueueHooks);
  bio_list_init(&asyncLayer->bios);
  asyncLayer->bioHook      = defaultBIOSubmitHook;
  asyncLayer->testThreadID = uds_get_thread_id();
  asyncLayer->syncLayer    = syncLayer;
  asyncLayer->state        = LAYER_INITIALIZED;

  layer = &asyncLayer->common;
  layer->getBlockCount          = getBlockCount;
  layer->allocateIOBuffer       = allocateIOBuffer;
  layer->reader                 = asyncReader;
  layer->writer                 = asyncWriter;
}

/**********************************************************************/
static void wrapOpenIndex(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  CU_ASSERT_STRING_EQUAL(vdo_get_dedupe_index_state_name(vdo->hash_zones),
                         "online");
  signalState(&asAsyncLayer()->indexOpen);
}

/**********************************************************************/
static bool openIndexHook(struct vdo_completion *completion)
{
  if (completion->type == VDO_HASH_ZONES_COMPLETION) {
    wrapCompletionCallback(completion, wrapOpenIndex);
    removeCompletionEnqueueHook(openIndexHook);
  }

  return true;
}

/**********************************************************************/
static void assertStartStopExpectation(int result)
{
  int expectation = asAsyncLayer()->startStopExpectation;

  if (expectation == VDO_READ_ONLY) {
    if (result != VDO_SUCCESS) {
      CU_ASSERT_EQUAL(result, VDO_READ_ONLY);
    }
    verifyReadOnly();
  } else {
    CU_ASSERT_EQUAL(result, expectation);
  }
}

/**
 * Process a single bio.
 *
 * @param bio  The bio to process
 *
 * @return  The result of processing the bio
 **/
static int processBIO(struct bio *bio)
{
    /*
     * Overload the REQ_NOIDLE flag to mean skip the check of the vdo admin
     * state. This is used by submit_bio_wait() for the geometry block read and
     * for synchronous flushes.
     */
  if ((bio->bi_flags & REQ_NOIDLE) != REQ_NOIDLE) {
    CU_ASSERT_FALSE(vdo_is_state_quiescent(&vdo->admin.state));
  }

  AsyncLayer *asyncLayer = asAsyncLayer();
  if ((asyncLayer->readOnly) && (bio_op(bio) != REQ_OP_READ)) {
    return -EROFS;
  }

  PhysicalLayer *ramLayer = getSynchronousLayer();
  if (((bio->bi_opf & REQ_PREFLUSH) == REQ_PREFLUSH)
      || (bio_op(bio) == REQ_OP_FLUSH)) {
    flushRAMLayer(ramLayer);
    if ((bio->bi_vcnt == 0) || (bio_op(bio) == REQ_OP_FLUSH)) {
      return VDO_SUCCESS;
    }
  }

  physical_block_number_t pbn = pbn_from_vio_bio(bio);
  assertNotInIndexRegion(pbn);

  int result;
  struct vio *vio = bio->bi_private;
  if (bio_data_dir(bio) == WRITE) {
    result = ramLayer->writer(ramLayer,
                              pbn,
                              vio->block_count,
                              (char *) bio->bi_io_vec->bv_page);
  } else {
    result = ramLayer->reader(ramLayer,
                              pbn,
                              vio->block_count,
                              (char *) bio->bi_io_vec->bv_page);
  }

  if (result != VDO_SUCCESS) {
    return result;
  }

  if ((bio->bi_opf & REQ_FUA) == REQ_FUA) {
    persistSingleBlockInRAMLayer(ramLayer, pbn);
  }

  return result;
}

/**********************************************************************/
static void drainBIOQueue(AsyncLayer *asyncLayer)
{
  struct bio_list bios;

  bio_list_init(&bios);
  while (!bio_list_empty(&asyncLayer->bios)) {
    bio_list_merge(&bios, &asyncLayer->bios);
    bio_list_init(&asyncLayer->bios);
    uds_unlock_mutex(&asyncLayer->mutex);
    while (!bio_list_empty(&bios)) {
      struct bio *bio = bio_list_pop(&bios);
      bio->bi_status = processBIO(bio);
      bio->bi_end_io(bio);
    }

    uds_lock_mutex(&asyncLayer->mutex);
  }
}

/**********************************************************************/
static void processBIOs(void *arg)
{
  AsyncLayer *asyncLayer = arg;
  WRITE_ONCE(asyncLayer->bioThreadID, pthread_self());
  uds_lock_mutex(&asyncLayer->mutex);
  do {
    while (asyncLayer->running && bio_list_empty(&asyncLayer->bios)) {
      uds_wait_cond(&asyncLayer->condition, &asyncLayer->mutex);
    }

    drainBIOQueue(asyncLayer);
  } while (asyncLayer->running);

  uds_unlock_mutex(&asyncLayer->mutex);
}

/**********************************************************************/
void startAsyncLayer(TestConfiguration configuration, bool loadVDO)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  assertOnTestThread();

  asyncLayer->running = true;
  VDO_ASSERT_SUCCESS(uds_create_thread(processBIOs,
                                       asyncLayer,
                                       "bio processor",
                                       &asyncLayer->bioThread));
  atomic64_set(&asyncLayer->requestCount, 0);
  asyncLayer->state = QUEUES_STARTED;

  struct dm_target *target;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct dm_target, __func__, &target));
  int result = loadTable(configuration, target);
  if (result != VDO_SUCCESS) {
    assertStartStopExpectation(result);
    stopAsyncLayer();
    UDS_FREE(target);
    return;
  }

  asyncLayer->state = TABLE_LOADED;

  if (!loadVDO) {
    return;
  }

  bool expectIndexOpen = ((asyncLayer->startStopExpectation == VDO_SUCCESS)
                          && configuration.deviceConfig.deduplication);
  if (expectIndexOpen) {
    asyncLayer->indexOpen = false;
    addCompletionEnqueueHook(openIndexHook);
  }

  result = vdoTargetType->preresume(target);
  assertStartStopExpectation(result);

  if (result != VDO_SUCCESS) {
    stopAsyncLayer();
    UDS_FREE(target);
    return;
  }

  if (expectIndexOpen) {
    waitForState(&asyncLayer->indexOpen);
  }

  asyncLayer->state = VDO_LOADED;
  vdoTargetType->resume(target);
}

/**********************************************************************/
void stopAsyncLayer(void)
{
  struct dm_target *target;
  AsyncLayer *asyncLayer = asAsyncLayer();
  assertOnTestThread();

  switch (asyncLayer->state) {
  case VDO_LOADED:
    CU_ASSERT_EQUAL(atomic64_read(&asyncLayer->requestCount), 0);
    if (!vdo_get_admin_state(vdo)->quiescent) {
      assertStartStopExpectation(suspendVDO(true));
    }

    fallthrough;

  case TABLE_LOADED:
    target = vdo->device_config->owning_target;
    vdoTargetType->dtr(target);
    UDS_FREE(UDS_FORGET(target));

    fallthrough;

  case QUEUES_STARTED:
    UDS_FORGET(vdo);
    if (asyncLayer->bioThread != NULL) {
      uds_lock_mutex(&asyncLayer->mutex);
      asyncLayer->running = false;
      uds_broadcast_cond(&asyncLayer->condition);
      uds_unlock_mutex(&asyncLayer->mutex);
      uds_join_threads(asyncLayer->bioThread);
    }
    fallthrough;

  default:
    // Flush the underlying layer
    flushSyncLayer();
  }

  asyncLayer->state = LAYER_INITIALIZED;
}

/**********************************************************************/
void setAsyncLayerReadOnly(bool readOnly)
{
  WRITE_ONCE(asAsyncLayer()->readOnly, readOnly);
  setStartStopExpectation((readOnly ? VDO_READ_ONLY : VDO_SUCCESS));
}

/**
 * The common callback used by all requests from the test thread.
 *
 * <p>Implements vdo_action.
 *
 * @param completion  the completion for the operation which has finished
 **/
static void requestDoneCallback(struct vdo_completion *completion)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  /*
   * By setting the callback to NULL, we indicate that this request is
   * complete. It would be nice if we could use the 'complete' field, but
   * unfortunately, that is set before we enter this method and so
   * awaitCompletion() would race with this method.
   */
  completion->callback = NULL;
  uds_broadcast_cond(&asyncLayer->condition);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**
 * Strip the wrapper from an action and run that action.
 *
 * Implements vdo_action.
 **/
static void requestCallback(struct vdo_completion *completion)
{
  struct vdo_completion *payload = completion->parent;
  UDS_FREE(UDS_FORGET(completion));

  vdo_action *action             = payload->callback;
  payload->callback              = requestDoneCallback;
  action(payload);
}

/**********************************************************************/
void launchAction(vdo_action *action, struct vdo_completion *completion)
{
  CU_ASSERT_PTR_NULL(completion->callback);
  completion->callback = action;

  atomic64_add(1, &(asAsyncLayer()->requestCount));

  struct vdo_completion *wrapper;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1,
                                  struct vdo_completion,
                                  __func__,
                                  &wrapper));
  vdo_initialize_completion(wrapper, vdo, VDO_TEST_COMPLETION);
  vdo_set_completion_callback_with_parent(wrapper,
                                          requestCallback,
                                          completion->callback_thread_id,
                                          completion);
  reallyEnqueueCompletion(wrapper);
}

/**********************************************************************/
int awaitCompletion(struct vdo_completion *completion)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  while (completion->callback != NULL) {
    uds_wait_cond(&asyncLayer->condition, &asyncLayer->mutex);
  }
  uds_unlock_mutex(&asyncLayer->mutex);
  atomic64_add(-1, &asyncLayer->requestCount);
  return completion->result;
}

/**********************************************************************/
int performAction(vdo_action *action, struct vdo_completion *completion)
{
  launchAction(action, completion);
  return awaitCompletion(completion);
}

/**********************************************************************/
void reallyEnqueueCompletion(struct vdo_completion *completion)
{
  vdo_enqueue_completion_with_priority(completion,
                                       (completion->priority | NO_HOOK_FLAG));
}

/**
 * Remove a function from the list of completion enqueue hooks while holding
 * the layer mutex. This method will update the canonical list of hooks and the
 * int_map, but will not modify the cache. Thus it is safe to call this method
 * from within a hook. The cache will be invalidated so that the next call to
 * runEnqueueHook() will repopulate it.
 *
 * @param function  The function to remove. It is not an error to remove a
 *                  function which is not currently registered
 **/
static void removeCompletionEnqueueHookLocked(CompletionHook *function)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  CompletionHookEntry *hook
    = int_map_remove(asyncLayer->completionEnqueueHooksMap,
                     (uintptr_t) function);
  if (hook != NULL) {
    list_del(&hook->listEntry);
    UDS_FREE(hook);
    asyncLayer->completionEnqueueHooksCacheValid = false;
  }
}

/**********************************************************************/
void removeCompletionEnqueueHook(CompletionHook *function)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  removeCompletionEnqueueHookLocked(function);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**
 * Remove all registered hooks while holding the layer mutex. This method will
 * update the canonical list of hooks and the int_map, but will not modify the
 * cache. Thus it is safe to call this method from within a hook. The cache
 * will be invalidated so that the next call to runEnqueueHook() will
 * repopulate it.
 **/
static void clearCompletionEnqueueHooksLocked(void)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  CompletionHookEntry *hook, *tmp;
  list_for_each_entry_safe(hook,
                           tmp,
                           &asyncLayer->completionEnqueueHooks,
                           listEntry) {
    removeCompletionEnqueueHookLocked(hook->function);
  }
}

/**********************************************************************/
void clearCompletionEnqueueHooks(void)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  clearCompletionEnqueueHooksLocked();
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**
 * Add a new function to the list of completion enqueue hooks while holding the
 * layer mutex. This method will update the canonical list of hooks and the
 * int_map, but will not modify the cache. Thus it is safe to call this method
 * from within a hook. The cache will be invalidated so that the next call to
 * runEnqueueHook() will repopulate it.
 *
 * @param function  The function to add. It is an error to add a function which
 *                  is already registered
 **/
static void addCompletionEnqueueHookLocked(CompletionHook *function)
{
  CompletionHookEntry *hook;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, CompletionHookEntry, __func__, &hook));
  hook->function = function;

  CompletionHookEntry *old;
  AsyncLayer *asyncLayer = asAsyncLayer();
  VDO_ASSERT_SUCCESS(int_map_put(asyncLayer->completionEnqueueHooksMap,
                                 (uintptr_t) function,
                                  hook,
                                  false,
                                  (void **) &old));
  CU_ASSERT_PTR_NULL(old);
  list_add_tail(&hook->listEntry, &asyncLayer->completionEnqueueHooks);
  asyncLayer->completionEnqueueHooksCacheValid = false;
}

/**********************************************************************/
void addCompletionEnqueueHook(CompletionHook *function)
{
  CU_ASSERT_PTR_NOT_NULL(function);
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  addCompletionEnqueueHookLocked(function);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**********************************************************************/
void setCompletionEnqueueHook(CompletionHook *function) {
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  clearCompletionEnqueueHooksLocked();
  addCompletionEnqueueHookLocked(function);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**********************************************************************/
void setCallbackFinishedHook(FinishedHook *function)
{
  asAsyncLayer()->callbackFinishedHook = function;
}

/**********************************************************************/
void clearLayerHooks(void)
{
  setCallbackFinishedHook(NULL);
  clearCompletionEnqueueHooks();
}

/**********************************************************************/
bool runEnqueueHook(struct vdo_completion *completion)
{
  if ((completion->parent == NULL) && isDataVIO(completion)) {
    struct data_vio *dataVIO = as_data_vio(completion);
    completion->parent = dataVIO->user_bio->unitTestContext;
  }

  if ((completion->priority & NO_HOOK_FLAG) == NO_HOOK_FLAG) {
    return true;
  }

  notifyEnqueue(completion);

  AsyncLayer *layer = asAsyncLayer();
  uds_lock_mutex(&layer->mutex);
  if (!layer->completionEnqueueHooksCacheValid) {
    // The cache is invalid due to an add or remove, so repopulate it.
    layer->completionEnqueueHooksCacheValid = true;
    layer->completionEnqueueHookCount = 0;
    CompletionHookEntry *hook;
    list_for_each_entry(hook, &layer->completionEnqueueHooks, listEntry) {
      CU_ASSERT(layer->completionEnqueueHookCount < MAX_HOOK_COUNT);
      layer->completionEnqueueHooksCache[layer->completionEnqueueHookCount++]
        = hook->function;
    }
  }
  uds_unlock_mutex(&layer->mutex);

  // Use the cache without holding the mutex so that hooks are free to modify
  // the hook configuration in a thread-safe manner.
  for (uint8_t i = layer->completionEnqueueHookCount; i > 0; i--) {
    if (!layer->completionEnqueueHooksCache[i - 1](completion)) {
      return false;
    }
  }

  return true;
}

/**********************************************************************/
void runFinishedHook(enum vdo_completion_priority priority)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  if ((priority & WORK_FLAG) == WORK_FLAG) {
    return;
  }

  if (asyncLayer->callbackFinishedHook != NULL) {
    asyncLayer->callbackFinishedHook();
  }
}

/**********************************************************************/
void setStartStopExpectation(int expectedResult)
{
  asAsyncLayer()->startStopExpectation = expectedResult;
}

/**********************************************************************/
void setBIOSubmitHook(BIOSubmitHook *function)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  asyncLayer->bioHook = ((function == NULL) ? defaultBIOSubmitHook : function);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**********************************************************************/
void reallyEnqueueBIO(struct bio *bio)
{
  if (((bio->bi_opf & REQ_PREFLUSH) == 0) || (bio->bi_iter.bi_size != 0)) {
    CU_ASSERT_PTR_NOT_NULL(bio->bi_private);
  }

  AsyncLayer *asyncLayer = asAsyncLayer();
  uds_lock_mutex(&asyncLayer->mutex);
  CU_ASSERT(asyncLayer->running);
  bio_list_add(&asyncLayer->bios, bio);
  uds_broadcast_cond(&asyncLayer->condition);
  uds_unlock_mutex(&asyncLayer->mutex);
}

/**********************************************************************/
void enqueueBIO(struct bio *bio)
{
  AsyncLayer *asyncLayer = asAsyncLayer();
  BIOSubmitHook *hook;
  uds_lock_mutex(&asyncLayer->mutex);
  hook = asyncLayer->bioHook;
  uds_unlock_mutex(&asyncLayer->mutex);

  if (hook(bio)) {
    reallyEnqueueBIO(bio);
  }
}

/**********************************************************************/
bool onBIOThread(void)
{
  return (pthread_self() == READ_ONCE(asAsyncLayer()->bioThreadID));
}
