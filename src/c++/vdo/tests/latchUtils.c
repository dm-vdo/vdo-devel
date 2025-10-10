/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "latchUtils.h"

#include <linux/list.h>

#include "memory-alloc.h"

#include "int-map.h"
#include "types.h"

#include "asyncLayer.h"
#include "asyncVIO.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static bool              initialized = false;
static struct int_map   *latchedVIOs = NULL;
static struct list_head  latches;
static WaitCondition    *waitCondition;
static LatchHook        *latchAttemptHook;
static LatchHook        *latchedVIOHook;

/*
 * We need to be able to pass the LatchExaminer into a locked method via the
 * context. Unfortunately, the compiler won't let us cast a void * to a
 * function pointer, so we need to wrap it in a struct.
 */
typedef struct {
  LatchExaminer *examiner;
} ExaminerStruct;

/**
 * Latch a VIO if requested.
 *
 * <p>Implements LockedMethod.
 **/
static bool latchVIO(void *context)
{
  struct vio *vio = context;
  if (latchAttemptHook != NULL) {
    latchAttemptHook(vio);
  }

  physical_block_number_t  pbn     = pbnFromVIO(vio);
  VIOLatch                *latched = vdo_int_map_get(latchedVIOs, pbn);
  if (latched == NULL) {
    return false;
  }

  CU_ASSERT_PTR_NULL(latched->vio);
  latched->vio = vio;
  if (latchedVIOHook != NULL) {
    latchedVIOHook(vio);
  }

  return true;
}

/**
 * Hook to block a VIO.
 *
 * Implements CompletionHook.
 **/
static bool attemptLatchVIO(struct vdo_completion *completion)
{
  if (!is_vio(completion)) {
    return true;
  }

  struct vio *vio = as_vio(completion);
  if ((waitCondition == NULL) || !waitCondition(vio)) {
    return true;
  }

  return !runLocked(latchVIO, vio);
}

/**********************************************************************/
void initializeLatchUtils(size_t         expectedEntries,
                          WaitCondition *condition,
                          LatchHook     *attemptHook,
                          LatchHook     *latchedHook)
{
  CU_ASSERT_EQUAL(initialized, false);
  INIT_LIST_HEAD(&latches);
  VDO_ASSERT_SUCCESS(vdo_int_map_create(expectedEntries, &latchedVIOs));
  waitCondition    = condition;
  latchAttemptHook = attemptHook;
  latchedVIOHook   = latchedHook;
  initialized      = true;
  setCompletionEnqueueHook(attemptLatchVIO);
}

/**********************************************************************/
void tearDownLatchUtils(void)
{
  if (!initialized) {
    return;
  }

  latchedVIOHook   = NULL;
  latchAttemptHook = NULL;
  waitCondition    = NULL;
  CU_ASSERT_EQUAL(vdo_int_map_size(latchedVIOs), 0);
  vdo_int_map_free(vdo_forget(latchedVIOs));
  CU_ASSERT(list_empty(&latches));
  initialized = false;
}

/**
 * Set up a latch while holding the mutex.
 *
 * <p>Implements LockedMethod.
 *
 * @param context  A pointer to the PBN to latch
 *
 * @return false (no broadcast)
 **/
static bool setLatchLocked(void *context)
{
  physical_block_number_t pbn = *((physical_block_number_t *) context);
  VIOLatch *latch;
  VDO_ASSERT_SUCCESS(vdo_allocate(1, __func__, &latch));
  latch->pbn = pbn;
  INIT_LIST_HEAD(&latch->latch_entry);
  list_add_tail(&latch->latch_entry, &latches);

  VIOLatch *prior;
  VDO_ASSERT_SUCCESS(vdo_int_map_put(latchedVIOs,
                                     pbn,
                                     latch,
                                     false,
                                     (void **) &prior));
  CU_ASSERT_PTR_NULL(prior);
  return false;
}

/**********************************************************************/
void setLatch(physical_block_number_t pbn)
{
  runLocked(setLatchLocked, &pbn);
}

/**
 * Clear a latch while holding the mutex. If a VIO was latched, it will be
 * released.
 *
 * <p>Implements LockedMethod.
 *
 * @param context  A pointer to the PBN of the latch to clear
 *
 * @return false (no broadcast)
 **/
static bool clearLatchLocked(void *context)
{
  VIOLatch *latch
    = vdo_int_map_remove(latchedVIOs, *((physical_block_number_t *) context));
  if (latch != NULL) {
    if (latch->vio != NULL) {
      reallyEnqueueVIO(latch->vio);
    }

    list_del(&latch->latch_entry);
    vdo_free(latch);
  }

  return false;
}

/**********************************************************************/
void clearLatch(physical_block_number_t pbn)
{
  runLocked(clearLatchLocked, &pbn);
}

/**
 * Implements WaitCondition.
 **/
static bool checkForBlockedVIO(void *context)
{
  VIOLatch *latched
    = vdo_int_map_get(latchedVIOs, *((physical_block_number_t *) context));
  CU_ASSERT_PTR_NOT_NULL(latched);
  return (latched->vio != NULL);
}

/**********************************************************************/
void waitForLatchedVIO(physical_block_number_t pbn)
{
  waitForCondition(checkForBlockedVIO, &pbn);
}

/**********************************************************************/
void releaseLatchedVIO(physical_block_number_t pbn)
{
  runOnCondition(checkForBlockedVIO, clearLatchLocked, &pbn);
}

/**********************************************************************/
bool releaseIfLatched(physical_block_number_t pbn)
{
  return runIfCondition(checkForBlockedVIO, clearLatchLocked, &pbn);
}

/**
 * Implements LockedMethod.
 **/
static bool examineLatchesLocked(void *context)
{
  LatchExaminer *examiner = ((ExaminerStruct *) context)->examiner;
  VIOLatch *latch;
  list_for_each_entry(latch, &latches, latch_entry) {
    if (examiner(latch)) {
      break;
    }
  }

  return false;
}

/**********************************************************************/
void examineLatches(LatchExaminer *examiner)
{
  ExaminerStruct examinerStruct = (ExaminerStruct) {
    .examiner = examiner,
  };
  runLocked(examineLatchesLocked, &examinerStruct);
}
