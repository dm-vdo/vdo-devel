/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */
/* Unit test implementation of kernel mempools */

#include <linux/mempool.h>

#include "memory-alloc.h"

#include "mutexUtils.h"
#include "vdoAsserts.h"

struct mempool {
  mempool_alloc_t  *constructor;
  mempool_free_t   *destructor;
  int               reserveLimit;
  int               reserveSize;
  int               outstandingEntries;
  void             *context;
  void             *reserve[];
};

/**********************************************************************/
mempool_t *mempool_create(int min_nr,
			  mempool_alloc_t *alloc_fn,
			  mempool_free_t *free_fn,
			  void *pool_data)
{
  mempool_t *pool;
  VDO_ASSERT_SUCCESS(vdo_allocate_extended(mempool_t,
                                           min_nr,
                                           void *,
                                           __func__,
                                           &pool));

  pool->constructor  = alloc_fn;
  pool->destructor   = free_fn;
  pool->reserveLimit = min_nr;
  pool->context      = pool_data;

  while (pool->reserveSize < pool->reserveLimit) {
    void *object = mempool_alloc(pool, 0);
    CU_ASSERT_PTR_NOT_NULL(object);
    mempool_free(object, pool);
  }

  return pool;
}

/**********************************************************************/
void mempool_destroy(mempool_t *pool)
{
  if (pool == NULL) {
    return;
  }

  CU_ASSERT_EQUAL(pool->outstandingEntries, 0);
  while (pool->reserveSize > 0) {
    pool->destructor(pool->reserve[--pool->reserveSize], pool->context);
  }

  vdo_free(pool);
}

void *mempool_alloc(mempool_t *pool, gfp_t gfp_mask)
{
  void *object = pool->constructor(gfp_mask, pool->context);
  if (object != NULL) {
    pool->outstandingEntries++;
    return object;
  }

  if (pool->reserveSize > 0) {
    pool->outstandingEntries++;
    return pool->reserve[--pool->reserveSize];
  }

  return NULL;
}

void mempool_free(void *element, mempool_t *pool)
{
  CU_ASSERT(pool->outstandingEntries > 0);

  pool->outstandingEntries--;
  if (pool->reserveSize < pool->reserveLimit) {
    pool->reserve[pool->reserveSize++] = element;
    return;
  }

  pool->destructor(element, pool->context);
}

