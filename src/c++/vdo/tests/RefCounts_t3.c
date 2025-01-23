/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "slab-depot.h"
#include "status-codes.h"
#include "vdo.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "callbackWrappingUtils.h"
#include "latchedCloseUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // Ensure multiple reference count blocks.
  SLAB_SIZE          = VDO_BLOCK_SIZE,
  SLAB_COUNT         = 1,
  JOURNAL_SIZE       = 2,
};

typedef struct poolCustomer {
  struct vdo_waiter      waiter;
  struct vdo_completion *wrapper;
  struct pooled_vio     *entry;
} PoolCustomer;

typedef struct {
  struct vdo_completion  completion;
  struct vio_pool       *pool;
  PoolCustomer           customer;
  struct pooled_vio     *entry;
} CustomerWrapper;

static struct vdo_completion completion;
static CustomerWrapper wrapper;

/**********************************************************************/
static void initializeRefCountsT3(void)
{
  TestParameters testParameters = {
    .slabSize = SLAB_SIZE,
    .slabJournalBlocks = JOURNAL_SIZE,
    .slabCount = SLAB_COUNT,
    .noIndexRegion = true,
  };
  initializeVDOTest(&testParameters);

  memset(&completion, 0, sizeof(completion));
}

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
  customer->entry = context;
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
static void initWrapper(struct vio_pool *pool, CustomerWrapper *wrapper)
{
  memset(wrapper, 0, sizeof(*wrapper));
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  wrapper->customer.waiter.callback = didAcquireVIO;
  wrapper->pool = pool;
}

/**********************************************************************/
static void doIngest(struct vdo_completion *completion)
{
  struct vio *vio = &wrapper.customer.entry->vio;
  struct vdo_slab *slab = vdo->depot->slabs[0];
  struct reference_block *block = &slab->reference_blocks[0];
  struct packed_reference_block *packed = (struct packed_reference_block *) vio->data;
  unsigned int allocated_count = 0;
  int i;
  
  vio->completion.parent = block;
  vio->io_size = VDO_BLOCK_SIZE;

  for (i = 0; i < VDO_SECTORS_PER_BLOCK; i++) {
    packed->sectors[i].commit_point.encoded_point = 0;
  }
  /*
   * Some patterns to examine: Big runs (over 256) of EMPTY and of
   * allocated, in case we don't correctly avoid overflow in
   * counting. Some PROVISIONAL, which should not show up in the
   * internalized version, and should be counted like EMPTY.
   */
  memset(packed->sectors[0].counts, EMPTY_REFERENCE_COUNT, COUNTS_PER_SECTOR);
  memset(packed->sectors[1].counts, PROVISIONAL_REFERENCE_COUNT, COUNTS_PER_SECTOR);
  memset(packed->sectors[2].counts, 3, COUNTS_PER_SECTOR);
  allocated_count += COUNTS_PER_SECTOR;
  for (i = 3; i < VDO_SECTORS_PER_BLOCK; i++) {
    for (int count = 0; count < COUNTS_PER_SECTOR; count += 2) {
      packed->sectors[i].counts[count] = 17;
      packed->sectors[i].counts[count+1] = EMPTY_REFERENCE_COUNT;
    }
    allocated_count += COUNTS_PER_SECTOR / 2;
  }

  block->slab->active_count += 1;
  finish_reference_block_load(&vio->completion);
  CU_ASSERT_EQUAL(allocated_count, block->allocated_count);
  CU_ASSERT_PTR_NULL(memchr(get_reference_counters_for_block(block),
                            PROVISIONAL_REFERENCE_COUNT, COUNTS_PER_BLOCK));

  vdo_finish_completion(completion);
}

/**
 * Most basic refCounts test.
 **/
static void testBasic(void)
{
  struct vio_pool *pool;

  memset(&completion, 0, sizeof(completion));

  VDO_ASSERT_SUCCESS(make_vio_pool(vdo, 1, 1, 0,
                                   VIO_TYPE_TEST, VIO_PRIORITY_METADATA,
                                   NULL, &pool));
  initWrapper(pool, &wrapper);
  acquireVIO(&wrapper);
  awaitCompletion(&wrapper.completion);

  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  performAction(doIngest, &completion);
}

/**********************************************************************/

static CU_TestInfo refCountsTests[] = {
  { "basic",                         testBasic                     },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo refCountsSuite = {
  .name                     = "reference counter tests (RefCounts_t3)",
  .initializer              = initializeRefCountsT3,
  .cleaner                  = tearDownVDOTest,
  .tests                    = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
