/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include <sys/resource.h>
#include <sys/time.h>

#include "albtest.h"

#include "memory-alloc.h"

#include "slab-depot.h"
#include "status-codes.h"
#include "vdo.h"
#include "vio.h"

#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  SLAB_SIZE          = 1 << MAX_VDO_SLAB_BITS,
  SLAB_COUNT         = 15,
  JOURNAL_SIZE       = 2,
  BLOCKS_PER_VIO     = 128,
  ITERATION_COUNT    = 500,
};

struct vio_wrapper {
  struct vdo_completion  completion;
  struct vdo_waiter      waiter;
  struct pooled_vio     *entry;
  unsigned int           allocated_count;
};

static struct vio_wrapper	*wrappers;
static int			 vio_count;
static struct vio_pool		*pool;

/**********************************************************************/
static void initializeRefCountsX1(void)
{
  TestParameters testParameters = {
    .slabSize = SLAB_SIZE,
    .slabJournalBlocks = JOURNAL_SIZE,
    .slabCount = SLAB_COUNT,
    .noIndexRegion = true,
  };

  initializeVDOTest(&testParameters);
  wrappers = NULL;
  pool = NULL;
}

/**********************************************************************/
static struct vio_wrapper *asWrapper(struct vdo_completion *wrapperCompletion)
{
  STATIC_ASSERT(offsetof(struct vio_wrapper, completion) == 0);
  return ((struct vio_wrapper *) wrapperCompletion);
}

/**********************************************************************/
static void didAcquireVIO(struct vdo_waiter *element, void *context)
{
  struct vio_wrapper *wrapper = container_of(element, struct vio_wrapper, waiter);

  wrapper->entry = context;
}

/**********************************************************************/
static void doAcquire(struct vdo_completion *wrapperCompletion)
{
  struct vio_wrapper *wrapper = asWrapper(wrapperCompletion);

  acquire_vio_from_pool(pool, &wrapper->waiter);
  vdo_finish_completion(wrapperCompletion);
}

/**********************************************************************/
static void acquireVIO(struct vio_wrapper *wrapper)
{
  vdo_reset_completion(&wrapper->completion);
  launchAction(doAcquire, &wrapper->completion);
}

/**********************************************************************/
static void doReturnVIO(struct vdo_completion *wrapperCompletion)
{
  struct vio_wrapper *wrapper = asWrapper(wrapperCompletion);

  return_vio_to_pool(wrapper->entry);
  vdo_finish_completion(wrapperCompletion);
}

/**********************************************************************/
static void returnVIO(struct pooled_vio *entry)
{
  struct vio_wrapper wrapper;

  wrapper.entry = entry;
  vdo_initialize_completion(&wrapper.completion, vdo, VDO_TEST_COMPLETION);
  performAction(doReturnVIO, &wrapper.completion);
}

/**********************************************************************/
static void initWrapper(struct vio_wrapper *wrapper)
{
  memset(wrapper, 0, sizeof(*wrapper));
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  wrapper->waiter.callback = didAcquireVIO;
}

/**********************************************************************/
static void initVIO(struct reference_block *block, struct vio *vio,
                    unsigned int *allocated_count_ptr)
{
  struct packed_reference_block *packed;
  // Most blocks will be nearly full, but make some mostly empty for some variety.
  bool mostly_empty = (random() & 7) == 0;
  int allocated_count = 0;

  vio->completion.parent = block;
  vio->io_size = VDO_BLOCK_SIZE * BLOCKS_PER_VIO;

  for (int block_number = 0; block_number < BLOCKS_PER_VIO; block_number++) {
    packed = (struct packed_reference_block *) (vio->data + block_number * VDO_BLOCK_SIZE);
    for (int sector_number = 0; sector_number < VDO_SECTORS_PER_BLOCK; sector_number++) {
      struct packed_reference_sector *sector = &packed->sectors[sector_number];
      for (int count_index = 0; count_index < COUNTS_PER_SECTOR; count_index++) {
        vdo_refcount_t value;

        if (mostly_empty && ((random() % 7) != 0)) {
          value = EMPTY_REFERENCE_COUNT;
        } else {
          value = random();
        }
        if (value != EMPTY_REFERENCE_COUNT && value != PROVISIONAL_REFERENCE_COUNT) {
          allocated_count++;
        }
        sector->counts[count_index] = value;
      }
    }
  }
  *allocated_count_ptr = allocated_count;
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

  start_time = cpuTime();
  for (int iter = 0; iter < iteration_count; iter++) {
    for (int i = 0; i < vio_count; i++) {
      struct vio *vio = &wrappers[i].entry->vio;
      struct reference_block *block = vio->completion.parent;
      u32 expected_alloc_count = wrappers[i].allocated_count;
      u32 allocated_count = 0;

      block->slab->active_count += BLOCKS_PER_VIO;
      finish_reference_block_load(&vio->completion);
      for (int j = 0; j < BLOCKS_PER_VIO; j++) {
        allocated_count += block[j].allocated_count;
      }
      CU_ASSERT_EQUAL(expected_alloc_count, allocated_count);
      wrappers[i].entry = NULL;
      acquire_vio_from_pool(pool, &wrappers[i].waiter);
      // We should always re-acquire the VIO immediately.
      CU_ASSERT_PTR_EQUAL(vio, wrappers[i].entry);
      // Fix cleared field.
      vio->completion.parent = block;
    }
  }
  end_time = cpuTime();

  unsigned long long cpu_usage = end_time - start_time;
  unsigned long long blocks_processed = iteration_count * vio_count * BLOCKS_PER_VIO;
  unsigned long long usage_per_block = cpu_usage * 1000 / blocks_processed;

  fprintf(stderr,
          "%u iterations, %u vios of %u blocks: cpu time = %llu.%06llu s, %llu ns per block\n",
          iteration_count, vio_count, BLOCKS_PER_VIO, cpu_usage / 1000000, cpu_usage % 1000000,
          usage_per_block);
  vdo_finish_completion(completion);
}

/**
 * Refcount loading performance test: CPU time
 **/
static void testBasic(void)
{
  int vios_per_slab = vdo->depot->slabs[0]->reference_block_count / BLOCKS_PER_VIO;
  int i;
  struct vdo_completion test_completion;

  vio_count = vios_per_slab * SLAB_COUNT;
  VDO_ASSERT_SUCCESS(vdo_allocate(vio_count, __func__, &wrappers));
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo, vio_count, BLOCKS_PER_VIO, 0,
                                   VIO_TYPE_TEST, VIO_PRIORITY_METADATA,
                                   NULL, &pool));
  for (i = 0; i < vio_count; i++) {
    initWrapper(&wrappers[i]);
    acquireVIO(&wrappers[i]);
    awaitCompletion(&wrappers[i].completion);
  }

  for (int slab_number = 0; slab_number < SLAB_COUNT; slab_number++) {
    struct vdo_slab *slab = vdo->depot->slabs[slab_number];
    for (i = 0; i < vios_per_slab; i++) {
      int wrapper_index = slab_number * vios_per_slab + i;
      struct vio *vio = &wrappers[wrapper_index].entry->vio;

      initVIO(&slab->reference_blocks[i * BLOCKS_PER_VIO], vio,
              &wrappers[wrapper_index].allocated_count);
    }
  }

  // do vio_pool stuff on the right worker thread
  vdo_initialize_completion(&test_completion, vdo, VDO_TEST_COMPLETION);
  performAction(doIngest, &test_completion);

  for (i = 0; i < vio_count; i++) {
    returnVIO(wrappers[i].entry);
  }
  free(wrappers);
  free_vio_pool(pool);
  pool = NULL;
}

/**********************************************************************/

static CU_TestInfo refCountsTests[] = {
  { "basic", testBasic },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo refCountsSuite = {
  .name        = "reference counter tests (RefCounts_x1)",
  .initializer = initializeRefCountsX1,
  .cleaner     = tearDownVDOTest,
  .tests       = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
