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
 * The waiter_callback registered in grabVIOs() to hold on
 * to a VIO pool entry so that it can later be returned to the pool.
 **/
static void saveVIOPoolEntry(struct waiter *waiter __attribute__((unused)),
                             void          *context)
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
  struct waiter waiter = (struct waiter) {
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
    return_vio_to_pool(poolAllocator->vio_pool, entry);
  }

  vdo_finish_completion(completion);
}

/**********************************************************************/
void returnVIOsToPool(void)
{
  if (viosToReserve > 0) {
    performSuccessfulActionOnThread(returnVIOPoolEntries,
				    poolAllocator->thread_id);
    viosToReserve = 0;
  }
}
