/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <sys/resource.h>
#include <sys/time.h>

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
#include "ptr-u32-map.h"
#include "ramLayer.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  SLAB_SIZE          = VDO_BLOCK_SIZE << 11,
  SLAB_COUNT         = 10,
  JOURNAL_SIZE       = 2,
  ITERATION_COUNT    = 500,
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

static struct vdo_completion  completion;
static CustomerWrapper       *wrappers;
static struct ptr_u32_map    *allocated_map;
static int                    vio_count;

/**********************************************************************/
static void initializeRefCountsT4(void)
{
  TestParameters testParameters = {
    .slabSize = SLAB_SIZE,
    .slabJournalBlocks = JOURNAL_SIZE,
    .slabCount = SLAB_COUNT,
    .noIndexRegion = true,
  };
  initializeVDOTest(&testParameters);

  memset(&completion, 0, sizeof(completion));
  wrappers = NULL;
  allocated_map = NULL;
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
static void initVIO(struct reference_block *block, struct vio *vio)
{
  struct packed_reference_block *packed = (struct packed_reference_block *) vio->data;
  bool mostly_empty = (random() & 7) == 0;
  int allocated_count = 0;

  vio->completion.parent = block; //&slab->reference_blocks[i];
  vio->io_size = VDO_BLOCK_SIZE;

  for (int s = 0; s < VDO_SECTORS_PER_BLOCK; s++) {
    struct packed_reference_sector *sector = &packed->sectors[s];
    sector->commit_point.encoded_point = 0;
    for (int count_index = 0; count_index < COUNTS_PER_SECTOR; count_index++) {
      vdo_refcount_t value;
      value = random();
      if (mostly_empty && ((random() % 7) != 3)) {
        value = EMPTY_REFERENCE_COUNT;
      }
      if (value != EMPTY_REFERENCE_COUNT && value != PROVISIONAL_REFERENCE_COUNT) {
        allocated_count++;
      }
      sector->counts[count_index] = value;
    }
  }
  VDO_ASSERT_SUCCESS(vdo_ptr_u32_map_put(allocated_map, vio, allocated_count, 1, NULL));
}

/**********************************************************************/
static uint64_t cpuTime(void)
{
  /* user cpu time */
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) < 0) {
    perror("getrusage");
    exit(1);
  }
  return ((uint64_t) ru.ru_utime.tv_sec * 1000000) + ru.ru_utime.tv_usec;
}

/**********************************************************************/
static void doIngest(struct vdo_completion *completion)
{
  uint64_t start_time, end_time;
  int iteration_count = ITERATION_COUNT;
  const char *env = getenv("ITERATION_COUNT");

  if (env) {
    iteration_count = atoi(env);
    if (iteration_count < 1)
      iteration_count = ITERATION_COUNT;
  }
  start_time = cpuTime();
  for (int iter = 0; iter < iteration_count; iter++) {
    for (int i = 0; i < vio_count; i++) {
      struct vio *vio = &wrappers[i].customer.entry->vio;
      u32 expected_alloc_count = vdo_ptr_u32_map_get(allocated_map, vio);
      struct reference_block *block = vio->completion.parent;

      block->slab->active_count += 1;
      finish_reference_block_load(&vio->completion);
      CU_ASSERT_EQUAL(expected_alloc_count, block->allocated_count);
      wrappers[i].customer.entry = NULL;
      acquire_vio_from_pool(wrappers[i].pool, &wrappers[i].customer.waiter);
      // We should always acquire a VIO immediately.
      CU_ASSERT_PTR_NOT_NULL(wrappers[i].customer.entry);
      // Fix cleared fields.
      vio = &wrappers[i].customer.entry->vio;
      vio->completion.parent = block;
    }
  }
  end_time = cpuTime();

  unsigned long long cpu_usage = end_time - start_time;
  unsigned long long usage_per_block = cpu_usage * 1000 / (iteration_count * vio_count);
  fprintf(stderr, "%u iterations, %u vios: cpu time = %llu.%06llu s, %llu ns per block\n",
          iteration_count, vio_count, cpu_usage / 1000000, cpu_usage % 1000000, usage_per_block);
  vdo_finish_completion(completion);
}

/**
 * Most basic refCounts test.
 **/
static void testBasic(void)
{
  int count_per_slab = vdo->depot->slabs[0]->reference_block_count;
  struct vio_pool *pool;
  int i;

  vio_count = count_per_slab * SLAB_COUNT;
  VDO_ASSERT_SUCCESS(vdo_allocate(vio_count, CustomerWrapper, __func__, &wrappers));
  VDO_ASSERT_SUCCESS(vdo_ptr_u32_map_create(0, &allocated_map));

  memset(&completion, 0, sizeof(completion));

  VDO_ASSERT_SUCCESS(make_vio_pool(vdo, vio_count, 1, 0,
                                   VIO_TYPE_TEST, VIO_PRIORITY_METADATA,
                                   NULL, &pool));
  for (i = 0; i < vio_count; i++) {
    initWrapper(pool, &wrappers[i]);
    acquireVIO(&wrappers[i]);
    awaitCompletion(&wrappers[i].completion);
  }

  for (int slab_number = 0; slab_number < SLAB_COUNT; slab_number++) {
    struct vdo_slab *slab = vdo->depot->slabs[slab_number];
    for (i = 0; i < count_per_slab; i++) {
      int wrapper_index = slab_number * count_per_slab + i;
      struct vio *vio = &wrappers[wrapper_index].customer.entry->vio;
      initVIO(&slab->reference_blocks[i], vio);
    }
  }

  // do stuff on the right thread
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  performAction(doIngest, &completion);

  for (i = 0; i < vio_count; i++) {
    returnVIO(pool, wrappers[i].customer.entry);
  }
  vdo_ptr_u32_map_free(allocated_map);
  free(wrappers);
}

/**********************************************************************/

static CU_TestInfo refCountsTests[] = {
  { "basic",                         testBasic                     },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo refCountsSuite = {
  .name                     = "reference counter tests (RefCounts_t4)",
  .initializer              = initializeRefCountsT4,
  .cleaner                  = tearDownVDOTest,
  .tests                    = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
