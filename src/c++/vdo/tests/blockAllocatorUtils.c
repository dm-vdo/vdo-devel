/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "blockAllocatorUtils.h"

#include <linux/list.h>

#include "block-allocator.h"
#include "kernel-types.h"

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
  struct vio_pool_entry *entry = context;
  list_move_tail(&entry->available_entry, &reservedVIOPoolEntries);
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
    vdo_acquire_block_allocator_vio(poolAllocator, &waiter);
    // Make sure that the waiter got a VIO synchronously.
    CU_ASSERT_TRUE(gotVIO);
  }

  vdo_complete_completion(completion);
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
  while (!list_empty(&reservedVIOPoolEntries)) {
    struct list_head *entry = reservedVIOPoolEntries.prev;
    list_del_init(entry);
    vdo_return_block_allocator_vio(poolAllocator,
                                   as_vio_pool_entry(entry));
  }

  vdo_finish_completion(completion, VDO_SUCCESS);
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
