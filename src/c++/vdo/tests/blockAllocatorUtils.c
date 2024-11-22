/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "blockAllocatorUtils.h"

#include <linux/list.h>

#include "slab-depot.h"
#include "types.h"

#include "vdoTestBase.h"
#include "vdoAsserts.h"

static struct list_head         reservedVIOPoolEntries;
static size_t                   viosToReserve = 0;
static struct block_allocator  *poolAllocator;
static bool                     gotVIO;

/**
 * The waiter_callback_fn registered in grabVIOs() to hold on
 * to a VIO pool entry so that it can later be returned to the pool.
 **/
static void saveVIOPoolEntry(struct vdo_waiter *waiter __attribute__((unused)),
                             void              *context)
{
  struct pooled_vio *pooled = context;
  list_add_tail(&pooled->list_entry, &reservedVIOPoolEntries);
  gotVIO = true;
}

/**
 * An action to grab some VIOs from the allocator's pool.
 **/
static void grabVIOs(struct vdo_completion *completion)
{
  struct vdo_waiter waiter = (struct vdo_waiter) {
    .next_waiter = NULL,
    .callback = saveVIOPoolEntry,
  };

  for (size_t i = 0; i < viosToReserve; i++) {
    gotVIO = false;
    acquire_vio_from_pool(poolAllocator->vio_pool, &waiter);
    // Make sure that the waiter got a VIO synchronously.
    CU_ASSERT_TRUE(gotVIO);
  }

  vdo_finish_completion(completion);
}

/**********************************************************************/
void reserveVIOsFromPool(struct block_allocator *allocator, size_t count)
{
  INIT_LIST_HEAD(&reservedVIOPoolEntries);
  poolAllocator = allocator;
  viosToReserve = count;
  performSuccessfulActionOnThread(grabVIOs, allocator->thread_id);
}

/**
 * Action to return the saved VIO pool entries
 *
 * @param completion  The completion for this action
 **/
static void returnVIOPoolEntries(struct vdo_completion *completion)
{
  struct pooled_vio *entry, *tmp;
  list_for_each_entry_safe_reverse(entry, tmp, &reservedVIOPoolEntries, list_entry) {
    list_del_init(&entry->list_entry);
    return_vio_to_pool(entry);
  }

  vdo_finish_completion(completion);
}

/**********************************************************************/
void returnVIOsToPool(void)
{
  if (viosToReserve > 0) {
    performSuccessfulActionOnThread(returnVIOPoolEntries, poolAllocator->thread_id);
    viosToReserve = 0;
  }
}

/**********************************************************************/
int getReferenceStatus(struct vdo_slab *slab,
                       physical_block_number_t pbn,
                       enum reference_status *statusPtr)
{
  vdo_refcount_t *counterPtr = NULL;
  int result = get_reference_counter(slab, pbn, &counterPtr);

  if (result != VDO_SUCCESS)
    return result;

  *statusPtr = reference_count_to_status(*counterPtr);
  return VDO_SUCCESS;
}

/**********************************************************************/
bool slabsHaveEquivalentReferenceCounts(struct vdo_slab *slabA, struct vdo_slab *slabB)
{
  size_t i;

  if ((slabA->block_count != slabB->block_count) ||
      (slabA->free_blocks != slabB->free_blocks) ||
      (slabA->reference_block_count != slabB->reference_block_count))
    return false;

  for (i = 0; i < slabA->reference_block_count; i++) {
    struct reference_block *block_a = slabA->reference_blocks + i;
    struct reference_block *block_b = slabB->reference_blocks + i;

    if (block_a->allocated_count != block_b->allocated_count)
      return false;
  }

  return (memcmp(slabA->counters,
                 slabB->counters,
                 sizeof(vdo_refcount_t) * slabA->block_count) == 0);
}

/**
 * A waiter_callback_fn to clean dirty reference blocks when resetting.
 *
 * @param block_waiter  The dirty block
 * @param context       Unused
 */
static void
clearDirtyReferenceBlocks(struct vdo_waiter *block_waiter, void *context __always_unused)
{
  container_of(block_waiter, struct reference_block, waiter)->is_dirty = false;
}

/**
 * vdo_reset_reference_counts() - Reset all reference counts back to RS_FREE.
 * @ref_counts: The reference counters to reset.
 */
void resetReferenceCounts(struct vdo_slab *slab)
{
  size_t i;

  memset(slab->counters, 0, slab->block_count * sizeof(vdo_refcount_t));
  slab->free_blocks = slab->block_count;
  slab->slab_journal_point = (struct journal_point) {
    .sequence_number = 0,
    .entry_count = 0,
  };

  for (i = 0; i < slab->reference_block_count; i++)
    slab->reference_blocks[i].allocated_count = 0;

  vdo_waitq_notify_all_waiters(&slab->dirty_blocks, clearDirtyReferenceBlocks, NULL);
}

/**
 * Convert a block number to the index of a reference counter for that block. Out of range values
 * are pinned to the beginning or one past the end of the array.
 *
 * @param slab  The slab
 * @param pbn   The physical block number to convert
 *
 * @return The index corresponding to the physical block number.
 */
static u64 pbnToIndex(const struct vdo_slab *slab, physical_block_number_t pbn)
{
  u64 index;

  if (pbn < slab->start) {
    return 0;
  }

  index = (pbn - slab->start);
  return min_t(u64, index, slab->block_count);
}

/**********************************************************************/
block_count_t countUnreferencedBlocks(struct vdo_slab *slab,
                                      physical_block_number_t start,
                                      physical_block_number_t end)
{
  block_count_t freeBlocks = 0;
  slab_block_number startIndex = pbnToIndex(slab, start);
  slab_block_number endIndex = pbnToIndex(slab, end);
  slab_block_number index;

  for (index = startIndex; index < endIndex; index++) {
    if (slab->counters[index] == EMPTY_REFERENCE_COUNT) {
      freeBlocks++;
    }
  }

  return freeBlocks;
}
