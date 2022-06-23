/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <stdarg.h>

#include "memory-alloc.h"
#include "random.h"

#include "pbn-lock-pool.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**
 * Assert that a pbn_lock is not null and consistent with an initialized lock
 * of the specified type.
 *
 * @param lock  The lock to check
 * @param type  The expected type
 **/
static void assertLockInitialized(const struct pbn_lock *lock,
                                  enum pbn_lock_type type)
{
  CU_ASSERT_PTR_NOT_NULL(lock);

  CU_ASSERT_EQUAL(0, lock->holder_count);

  // Can only check type field indirectly, so just check one property.
  CU_ASSERT_EQUAL((type == VIO_READ_LOCK), vdo_is_pbn_read_lock(lock));
}

/**
 * Borrow a lock from the pool, asserting success, verify it was initialized,
 * then corrupt every byte of it. The returned lock pointer must only be used
 * to return the lock to the pool.
 *
 * @param pool  The pool from which to borrow
 * @param type  The lock type to use for initialization
 *
 * @return the borrowed lock, overwritten with all 0xff bytes
 **/
static struct pbn_lock *borrow(struct pbn_lock_pool *pool,
                               enum pbn_lock_type type)
{
  struct pbn_lock *lock;
  VDO_ASSERT_SUCCESS(vdo_borrow_pbn_lock_from_pool(pool, type, &lock));
  assertLockInitialized(lock, type);

  // Overwrite the lock structure completely to ensure the pool doesn't
  // use any of it while it's on loan.
  memset(lock, 0xff, sizeof(*lock));

  return lock;
}

/**
 * Attempt to borrow a lock from the pool, asserting that it fails
 * with a lock error.
 **/
static void failBorrow(struct pbn_lock_pool *pool)
{
  struct pbn_lock *lock = NULL;
  CU_ASSERT_EQUAL(VDO_LOCK_ERROR, vdo_borrow_pbn_lock_from_pool(pool,
                                                                VIO_READ_LOCK,
                                                                &lock));
  CU_ASSERT_PTR_NULL(lock);
}

/**
 * Return a lock to the pool, first initializing it so error checks in the
 * pool code won't fail because of the memory smashing in borrow().
 **/
static void returnLock(struct pbn_lock_pool *pool, struct pbn_lock *lock)
{
  vdo_initialize_pbn_lock(lock, VIO_READ_LOCK);
  vdo_return_pbn_lock_to_pool(pool, lock);
}

/**
 * Simple test of a pool with two locks.
 **/
static void testPBNLockPool(void)
{
  // Make a pool with two locks.
  struct pbn_lock_pool *pool;
  VDO_ASSERT_SUCCESS(vdo_make_pbn_lock_pool(2, &pool));

  // Borrow them both.
  struct pbn_lock *lock1 = borrow(pool, VIO_READ_LOCK);
  struct pbn_lock *lock2 = borrow(pool, VIO_WRITE_LOCK);

  // Make sure we can't borrow more (twice to catch '==' errors).
  failBorrow(pool);
  failBorrow(pool);

  // Put one back, then we should be able to get it again.
  returnLock(pool, UDS_FORGET(lock1));
  lock1 = borrow(pool, VIO_WRITE_LOCK);

  // Pool should be empty again.
  failBorrow(pool);

  // Return both locks and free the pool.
  returnLock(pool, UDS_FORGET(lock1));
  returnLock(pool, UDS_FORGET(lock2));
  vdo_free_pbn_lock_pool(pool);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "simple pbn_lock_pool test", testPBNLockPool },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "PBNLockPool_t1",
  .initializer = NULL,
  .cleaner     = NULL,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
