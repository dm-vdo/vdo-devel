/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "physical-zone.h"
#include "types.h"
#include "vdo.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

static bool                   readOnMatch;
static data_vio_count_t       count;
static struct pbn_lock      **locks;
static struct pbn_lock        saved;
static struct physical_zone  *zone;


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
 * Implements vdo_action_fn
 **/
static void borrow(struct vdo_completion *completion)
{
  enum pbn_lock_type type
    = ((((count % 2) == 0) == readOnMatch) ? VIO_READ_LOCK : VIO_WRITE_LOCK);

  VDO_ASSERT_SUCCESS(vdo_attempt_physical_zone_pbn_lock(zone,
                                                        count,
                                                        type,
                                                        &locks[count]));
  assertLockInitialized(locks[count], type);

  if (count == 0) {
    memcpy(&saved, locks[count], sizeof(struct pbn_lock));
  }

  // Overwrite the lock structure completely to ensure the pool doesn't
  // use any of it while it's on loan.
  memset(locks[count], 0xff, sizeof(struct pbn_lock));
  vdo_finish_completion(completion);
}

/**
 * Attempt to borrow a lock from the pool, asserting that it fails
 * with a lock error.
 *
 * Implements vdo_action_fn
 **/
static void failBorrow(struct vdo_completion *completion)
{
  struct pbn_lock *lock = NULL;
  set_exit_on_assertion_failure(false);
  int result = vdo_attempt_physical_zone_pbn_lock(zone,
                                                  count,
                                                  VIO_READ_LOCK,
                                                  &lock);
  set_exit_on_assertion_failure(true);
  CU_ASSERT_EQUAL(VDO_LOCK_ERROR, result);
  CU_ASSERT_PTR_NULL(lock);
  vdo_finish_completion(completion);
}

/**
 * Return a lock to the pool, first initializing it so error checks in the
 * pool code won't fail because of the memory smashing in borrow().
 *
 * Implements vdo_action_fn
 **/
static void returnLock(struct vdo_completion *completion)
{
  struct pbn_lock *lock = uds_forget(locks[count]);

  memcpy(lock, &saved, sizeof(struct pbn_lock));
  lock->holder_count = 1;
  vdo_release_physical_zone_pbn_lock(zone, count, lock);
  vdo_finish_completion(completion);
}

/**
 * Simple test of a pool with two locks.
 **/
static void testPBNLockPool(void)
{
  zone = &vdo->physical_zones->zones[0];
  VDO_ASSERT_SUCCESS(uds_allocate(MAXIMUM_VDO_USER_VIOS * 2,
                                  struct pbn_lock *,
                                  __func__,
                                  &locks));

  // Borrow all the locks.
  readOnMatch = true;
  for (count = 0; count < MAXIMUM_VDO_USER_VIOS * 2; count++) {
    performSuccessfulActionOnThread(borrow, zone->thread_id);
  }

  // Make sure we can't borrow more (twice to catch '==' errors).
  performSuccessfulActionOnThread(failBorrow, zone->thread_id);
  performSuccessfulActionOnThread(failBorrow, zone->thread_id);

  // Put one back, then we should be able to get it again.
  count = 0;
  readOnMatch = false;
  performSuccessfulActionOnThread(returnLock, zone->thread_id);
  performSuccessfulActionOnThread(borrow, zone->thread_id);

  // Pool should be empty again.
  performSuccessfulActionOnThread(failBorrow, zone->thread_id);

  // Return all locks.
  for (count = 0; count < MAXIMUM_VDO_USER_VIOS * 2; count++) {
    performSuccessfulActionOnThread(returnLock, zone->thread_id);
  }

  uds_free(locks);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "simple pbn_lock_pool test", testPBNLockPool },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "PBNLockPool_t1",
  .initializer = initializeDefaultVDOTest,
  .cleaner     = tearDownVDOTest,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
