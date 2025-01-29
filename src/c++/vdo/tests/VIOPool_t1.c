/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <stdarg.h>

#include "permassert.h"

#include "vio.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
enum {
  CUSTOMERS    = 3,
  POOL_SIZE    = 15,
  MAX_PER_CUST = POOL_SIZE,
};

typedef struct poolCustomer {
  struct vdo_waiter      waiter;
  struct vdo_completion *wrapper;
  struct pooled_vio     *entries[MAX_PER_CUST + 1];
  size_t                 using;
} PoolCustomer;

typedef struct {
  struct vdo_completion  completion;
  struct vio_pool       *pool;
  PoolCustomer           customer;
  struct pooled_vio     *entry;
} CustomerWrapper;

/**********************************************************************/
static CustomerWrapper *asWrapper(struct vdo_completion *wrapperCompletion)
{
  STATIC_ASSERT(offsetof(CustomerWrapper, completion) == 0);
  return ((CustomerWrapper *) wrapperCompletion);
}

/**********************************************************************/
static void didAcquireVIO(struct vdo_waiter *element, void *context)
{
  PoolCustomer *customer = container_of(element, PoolCustomer, waiter);
  customer->entries[customer->using++] = context;
}

/**********************************************************************/
static void doAcquire(struct vdo_completion *wrapperCompletion)
{
  CustomerWrapper *wrapper = asWrapper(wrapperCompletion);
  acquire_vio_from_pool(wrapper->pool, &wrapper->customer.waiter);
  vdo_finish_completion(wrapperCompletion);
}

/**********************************************************************/
static void acquireVIO(CustomerWrapper *wrapper)
{
  vdo_reset_completion(&wrapper->completion);
  launchAction(doAcquire, &wrapper->completion);
}

/**********************************************************************/
static void doReturnVIO(struct vdo_completion *wrapperCompletion)
{
  CustomerWrapper *wrapper = asWrapper(wrapperCompletion);
  return_vio_to_pool(wrapper->entry);
  vdo_finish_completion(wrapperCompletion);
}

/**********************************************************************/
static void returnVIO(struct vio_pool *pool, struct pooled_vio *entry)
{
  CustomerWrapper wrapper;
  wrapper.pool  = pool;
  wrapper.entry = entry;
  vdo_initialize_completion(&wrapper.completion, vdo, VDO_TEST_COMPLETION);
  performAction(doReturnVIO, &wrapper.completion);
}

/**********************************************************************/
static void initWrapper(struct vio_pool *pool, CustomerWrapper *wrapper)
{
  memset(wrapper, 0, sizeof(*wrapper));
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  wrapper->customer.waiter.callback = didAcquireVIO;
  wrapper->pool = pool;
}

/**********************************************************************/
static void testVIOPool(void)
{
  struct vio_pool *pool;
  static const size_t poolSize = 5;
  int vio_size;

  for (vio_size = 1; vio_size <= 100; vio_size *= 10) {
    VDO_ASSERT_SUCCESS(make_vio_pool(vdo,
                                     poolSize,
                                     vio_size,
                                     0,
                                     VIO_TYPE_TEST,
                                     VIO_PRIORITY_METADATA,
                                     NULL,
                                     &pool));
    CU_ASSERT_PTR_NOT_NULL(pool);

    CustomerWrapper wrappers[7];
    for (size_t i = 0; i < ARRAY_SIZE(wrappers); i++) {
      initWrapper(pool, &wrappers[i]);
      acquireVIO(&wrappers[i]);
    }

    for (size_t i = 0; i < poolSize; i++) {
      awaitCompletion(&wrappers[i].completion);
      CU_ASSERT_EQUAL(wrappers[i].customer.entries[0]->vio.block_count, vio_size);
    }

    returnVIO(pool, wrappers[0].customer.entries[0]);
    awaitCompletion(&wrappers[5].completion);
    CU_ASSERT_EQUAL(wrappers[5].customer.entries[0]->vio.block_count, vio_size);

    returnVIO(pool, wrappers[1].customer.entries[0]);
    awaitCompletion(&wrappers[6].completion);
    CU_ASSERT_EQUAL(wrappers[6].customer.entries[0]->vio.block_count, vio_size);

    returnVIO(pool, wrappers[2].customer.entries[0]);
    returnVIO(pool, wrappers[3].customer.entries[0]);
    returnVIO(pool, wrappers[4].customer.entries[0]);
    returnVIO(pool, wrappers[5].customer.entries[0]);
    returnVIO(pool, wrappers[6].customer.entries[0]);

    free_vio_pool(pool);
  }
}

/**********************************************************************/
static void returnSomeCustomerEntries(CustomerWrapper *wrapper, size_t count)
{
  PoolCustomer *customer = &wrapper->customer;
  for (size_t i = 0; i < count; i++) {
    returnVIO(wrapper->pool, customer->entries[i]);
  }
  if (count < customer->using) {
    memmove(customer->entries, &customer->entries[count],
            (customer->using - count) * sizeof(*customer->entries));
  }
  customer->using -= count;
}

/**********************************************************************/
static void getSomeCustomerEntries(CustomerWrapper *wrapper,
                                   size_t           count)
{
  for (size_t n = 0; n < count; n++) {
    acquireVIO(wrapper);
    awaitCompletion(&wrapper->completion);
  }
}

/**********************************************************************/
static void cleanUpCustomer(CustomerWrapper *wrapper)
{
  returnSomeCustomerEntries(wrapper, wrapper->customer.using);
}

/**********************************************************************/
static void checkExpectations(CustomerWrapper *wrappers, va_list ap)
{
  for (size_t i = 0; i < CUSTOMERS; i++) {
    size_t expect = va_arg(ap, size_t);
    CU_ASSERT_EQUAL(wrappers[i].customer.using, expect);
  }
}

/**
 * Acquire some entries on behalf of the specified customer and verify counts.
 *
 * @param wrappers           The customer wrapper array of length CUSTOMERS
 * @param cust               The customer number
 * @param count              How many to acquire
 * @param ...                A count of how many entries each customer
 *                             is expected to hold, must be CUSTOMERS
 *                             number of parameters.
 **/
static void checkAcquire(CustomerWrapper *wrappers,
                         size_t           cust,
                         size_t           count,
                         ...)
{
  getSomeCustomerEntries(&wrappers[cust], count);
  va_list ap;
  va_start(ap, count);
  checkExpectations(wrappers, ap);
  va_end(ap);
}

/**
 * Release some entries on behalf of the specified customer and verify counts.
 *
 * @param customers     The customer wrapper array of length CUSTOMERS
 * @param cust          The customer number
 * @param count         How many to release
 * @param ...           A count of how many entries each customer
 *                        is expected to hold, must be CUSTOMERS
 *                        number of parameters.
 **/
static void checkRelease(CustomerWrapper *wrappers,
                         size_t           cust,
                         size_t           count,
                         ...)
{
  returnSomeCustomerEntries(&wrappers[cust], count);
  va_list ap;
  va_start(ap, count);
  checkExpectations(wrappers, ap);
  va_end(ap);
}

/**********************************************************************/
static void testReuseCompletions(void)
{
  struct vio_pool *pool;
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo,
                                   POOL_SIZE,
                                   1,
                                   0,
                                   VIO_TYPE_TEST,
                                   VIO_PRIORITY_METADATA,
                                   NULL,
                                   &pool));

  CustomerWrapper customers[CUSTOMERS];
  for (size_t i = 0; i < ARRAY_SIZE(customers); i++) {
    initWrapper(pool, &customers[i]);
  }

  checkAcquire(customers, 0, 10, 10,  0, 0);    //  5 - - -
  checkAcquire(customers, 1,  6, 10,  5, 0);    //  0 - W -
  checkAcquire(customers, 2,  1, 10,  5, 0);    //  0 - W W

  checkRelease(customers, 0,  5,  5,  6, 1);    //  3 - - -

  checkAcquire(customers, 1,  4,  5,  9, 1);    //  0 - W -

  checkRelease(customers, 0,  5,  0, 10, 1);    //  4 - - -

  checkAcquire(customers, 2,  5,  0, 10, 5);    //  0 - - W

  checkRelease(customers, 1,  3,  0,  7, 6);    //  2 - - -

  checkAcquire(customers, 0,  2,  2,  7, 6);    //  0 - - -
  checkAcquire(customers, 1,  1,  2,  7, 6);    //  0 - W -
  checkAcquire(customers, 2,  1,  2,  7, 6);    //  0 - W W
  checkAcquire(customers, 0,  1,  2,  7, 6);    //  0 W W W

  // Verify that a waiter can release a vio and re-acquire it.
  checkRelease(customers, 1,  1,  2,  7, 6);    //  0 W - W
  checkRelease(customers, 1,  3,  3,  4, 7);    //  1 - - -

  for (size_t i = 0; i < CUSTOMERS; i++) {
    cleanUpCustomer(&customers[i]);
  }

  free_vio_pool(pool);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "vio pool",          testVIOPool          },
  { "reuse completions", testReuseCompletions },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "VIOPool_t1",
  .initializer = initializeDefaultBasicTest,
  .cleaner     = tearDownVDOTest,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
