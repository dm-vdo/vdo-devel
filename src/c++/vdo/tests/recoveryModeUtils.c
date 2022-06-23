/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "recoveryModeUtils.h"

#include "memory-alloc.h"
#include "uds-threads.h"

#include "ref-counts.h"
#include "slab-depot.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef enum {
  LATCH_UNSET,
  LATCH_READ,
  LATCH_WRITE,
} LatchOperation;

static struct vio      *LATCH_DESIRED = (struct vio *) 0xffffffffffffffff;
static LatchOperation   latchOperation;
static struct int_map  *latchedVIOs;
static struct mutex     mutex;
static struct cond_var  condition;

/**********************************************************************/
void initializeRecoveryModeTest(const TestParameters *testParameters)
{
  VDO_ASSERT_SUCCESS(uds_init_mutex(&mutex));
  VDO_ASSERT_SUCCESS(uds_init_cond(&condition));
  VDO_ASSERT_SUCCESS(make_int_map(8, 0, &latchedVIOs));
  initializeVDOTest(testParameters);
}

/**********************************************************************/
void tearDownRecoveryModeTest(void)
{
  tearDownVDOTest();
  free_int_map(UDS_FORGET(latchedVIOs));
  uds_destroy_cond(&condition);
  uds_destroy_mutex(&mutex);
}

/**********************************************************************/
static bool latchSlab(struct vio              *vio,
                      slab_count_t             slabNumber,
                      physical_block_number_t  pbn)
{
  struct vio *latchedSlabVIO = int_map_get(latchedVIOs, slabNumber);
  if (latchedSlabVIO != LATCH_DESIRED) {
    return true;
  }

  struct slab_depot *depot     = vdo->depot;
  struct ref_counts *refCounts = depot->slabs[slabNumber]->reference_counts;

  // Reference count may not exist when the hook is called.
  if ((refCounts == NULL)
      || (pbn < refCounts->origin)
      || (pbn >= refCounts->origin + refCounts->reference_block_count)) {
    return true;
  }

  UDS_ASSERT_SUCCESS(int_map_put(latchedVIOs, slabNumber, vio, true, NULL));
  uds_broadcast_cond(&condition);
  return false;
}

/**
 * Latch the reference count IO that is part of the slab rebuild process for a
 * specific slab.
 *
 * Implements CompletionHook.
 **/
static bool latchReferenceBlockIO(struct vdo_completion *completion)
{
  if (!vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL) || !onBIOThread()) {
    return true;
  }

  switch (latchOperation) {
  case LATCH_WRITE:
    if (!isMetadataWrite(completion)) {
      return true;
    }

    break;

  case LATCH_READ:
    if (!isMetadataRead(completion)) {
      return true;
    }

    break;

  default:
    return true;
  }

  struct vio *vio = as_vio(completion);
  physical_block_number_t pbn = vio->physical;
  slab_count_t slabNumber;
  VDO_ASSERT_SUCCESS(vdo_get_slab_number(vdo->depot, pbn, &slabNumber));
  uds_lock_mutex(&mutex);
  bool result = latchSlab(vio, slabNumber, pbn);
  uds_unlock_mutex(&mutex);
  return result;
}

/**********************************************************************/
static void setupSlabLatch(slab_count_t slabNumber, LatchOperation operation)
{
  CU_ASSERT_TRUE((latchOperation == operation)
                 || (latchOperation == LATCH_UNSET));

  uds_lock_mutex(&mutex);
  struct vio *oldEntry = NULL;
  UDS_ASSERT_SUCCESS(int_map_put(latchedVIOs, slabNumber, LATCH_DESIRED,
                                 false, (void **) &oldEntry));

  // Fail if we attempted to override an existing entry
  CU_ASSERT_PTR_NULL(oldEntry);

  latchOperation = operation;
  setCompletionEnqueueHook(latchReferenceBlockIO);
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
void setupSlabScrubbingLatch(slab_count_t slabNumber)
{
  setupSlabLatch(slabNumber, LATCH_WRITE);
}

/**********************************************************************/
void latchAnyScrubbingSlab(slab_count_t slabs)
{
  for (slab_count_t i = 0; i < slabs; i++) {
    setupSlabScrubbingLatch(i);
  }
}

/**********************************************************************/
void setupSlabLoadingLatch(slab_count_t slabNumber)
{
  setupSlabLatch(slabNumber, LATCH_READ);
}

/**********************************************************************/
static bool isSlabLatched(slab_count_t slabNumber, struct vio **latchedVIO)
{
  struct vio *latchedSlabVIO = int_map_get(latchedVIOs, slabNumber);
  bool isLatched = ((latchedSlabVIO != NULL)
                    && (latchedSlabVIO != LATCH_DESIRED));
  if (isLatched && (latchedVIO != NULL)) {
    *latchedVIO = latchedSlabVIO;
  }

  return isLatched;
}

/**********************************************************************/
void waitForSlabLatch(slab_count_t slabNumber)
{
  uds_lock_mutex(&mutex);
  while (!isSlabLatched(slabNumber, NULL)) {
    uds_wait_cond(&condition, &mutex);
  }
  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
slab_count_t waitForAnySlabToLatch(slab_count_t slabs)
{
  slab_count_t latchedSlab;
  uds_lock_mutex(&mutex);
  for (bool slabLatched = false; !slabLatched;) {
    for (latchedSlab = 0; latchedSlab < slabs; latchedSlab++) {
      if (isSlabLatched(latchedSlab, NULL)) {
        slabLatched = true;
        break;
      }
    }
    if (!slabLatched) {
      uds_wait_cond(&condition, &mutex);
    }
  }
  uds_unlock_mutex(&mutex);
  return latchedSlab;
}

/**********************************************************************/
void releaseSlabLatch(slab_count_t slabNumber)
{
  uds_lock_mutex(&mutex);
  struct vio *latchedSlabVIO = int_map_remove(latchedVIOs, slabNumber);
  if (int_map_size(latchedVIOs) == 0) {
    removeCompletionEnqueueHook(latchReferenceBlockIO);
    latchOperation = LATCH_UNSET;
  }
  uds_unlock_mutex(&mutex);

  CU_ASSERT_PTR_NOT_NULL(latchedSlabVIO);
  CU_ASSERT_FALSE(latchedSlabVIO == LATCH_DESIRED);
  reallyEnqueueVIO(latchedSlabVIO);
}

/**********************************************************************/
void releaseAllSlabLatches(slab_count_t slabs)
{
  uds_lock_mutex(&mutex);

  removeCompletionEnqueueHook(latchReferenceBlockIO);
  latchOperation = LATCH_UNSET;

  for (slab_count_t i = 0; i < slabs; i++) {
    struct vio *latchedSlabVIO = int_map_remove(latchedVIOs, i);
    if ((latchedSlabVIO != NULL) && (latchedSlabVIO != LATCH_DESIRED)) {
      reallyEnqueueVIO(latchedSlabVIO);
    }
  }

  uds_unlock_mutex(&mutex);
}

/**********************************************************************/
void injectErrorInLatchedSlab(slab_count_t slabNumber, int errorCode)
{
  uds_lock_mutex(&mutex);
  struct vio *latchedVIO;
  CU_ASSERT_TRUE(isSlabLatched(slabNumber, &latchedVIO));
  setVIOResult(latchedVIO, errorCode);
  uds_unlock_mutex(&mutex);
}
