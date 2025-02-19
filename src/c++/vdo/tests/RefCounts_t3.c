/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

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
  // Ensure multiple reference count blocks.
  SLAB_SIZE          = VDO_BLOCK_SIZE,
  SLAB_COUNT         = 1,
  JOURNAL_SIZE       = 2,
};

struct vio_wrapper {
  struct vdo_completion  completion;
  struct vio_pool       *pool;
  struct vdo_waiter      waiter;
  struct pooled_vio     *entry;
};

static struct vdo_completion	 completion;
static struct vio_wrapper	 wrapper;
static struct vio_pool		*pool;

static void didAcquireVIO(struct vdo_waiter *element, void *context);

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
  VDO_ASSERT_SUCCESS(make_vio_pool(vdo, 1, 1, 0,
                                   VIO_TYPE_TEST, VIO_PRIORITY_METADATA,
                                   NULL, &pool));
  memset(&wrapper, 0, sizeof(wrapper));
  vdo_initialize_completion(&wrapper.completion, vdo, VDO_TEST_COMPLETION);
  wrapper.waiter.callback = didAcquireVIO;
  wrapper.pool = pool;

  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
}

/**********************************************************************/
static void didAcquireVIO(struct vdo_waiter *element, void *context)
{
  struct vio_wrapper *wrapper = container_of(element, struct vio_wrapper, waiter);

  wrapper->entry = context;
}

/**********************************************************************/
static void doIngest(u8 byte1, u8 byte2, unsigned int expected_count)
{
  /*
   * Fill each sector's counter values with alternating byte1 and
   * byte2. If they're the same value, that value fills the whole
   * block.
   */
  struct vio *vio;
  struct vdo_slab *slab = vdo->depot->slabs[0];
  struct reference_block *block = &slab->reference_blocks[0];
  struct packed_reference_block *packed;
  int i;

  wrapper.entry = NULL;
  acquire_vio_from_pool(wrapper.pool, &wrapper.waiter);
  CU_ASSERT_PTR_NOT_NULL(wrapper.entry);
  vio = &wrapper.entry->vio;
  vio->completion.parent = block;
  vio->io_size = VDO_BLOCK_SIZE;
  packed = (struct packed_reference_block *) vio->data;

  for (i = 0; i < VDO_SECTORS_PER_BLOCK; i++) {
    for (int count = 0; count < COUNTS_PER_SECTOR; count += 2) {
      packed->sectors[i].counts[count] = byte1;
      packed->sectors[i].counts[count + 1] = byte2;
    }
  }

  block->slab->active_count += 1;
  finish_reference_block_load(&vio->completion);
  CU_ASSERT_EQUAL(expected_count, block->allocated_count);
  /* If PROVISIONAL was specified, it should have been cleared. */
  CU_ASSERT_PTR_NULL(memchr(get_reference_counters_for_block(block),
                            PROVISIONAL_REFERENCE_COUNT, COUNTS_PER_BLOCK));
}

/**********************************************************************/
static void doTest(struct vdo_completion *completion)
{
  u8 allocated = 3;

  /*
   * We need an "allocated" value, sanity check the number we pick
   * isn't one of the special values.
   */
  CU_ASSERT_NOT_EQUAL(allocated, EMPTY_REFERENCE_COUNT);
  CU_ASSERT_NOT_EQUAL(allocated, PROVISIONAL_REFERENCE_COUNT);
  /*
   * Some patterns to examine: Lots of EMPTY and lots of allocated, in
   * case we don't correctly avoid overflow in counting. Some
   * PROVISIONAL, which should not show up in the internalized version,
   * and should be counted like EMPTY.
   */
  doIngest(EMPTY_REFERENCE_COUNT, EMPTY_REFERENCE_COUNT, 0);
  doIngest(PROVISIONAL_REFERENCE_COUNT, PROVISIONAL_REFERENCE_COUNT, 0);
  doIngest(allocated, allocated, COUNTS_PER_BLOCK);
  doIngest(EMPTY_REFERENCE_COUNT, allocated, COUNTS_PER_BLOCK / 2);
  /* Mix provisional and other, to make sure we don't clobber the other. */
  doIngest(PROVISIONAL_REFERENCE_COUNT, EMPTY_REFERENCE_COUNT, 0);
  doIngest(PROVISIONAL_REFERENCE_COUNT, allocated, COUNTS_PER_BLOCK / 2);

  vdo_finish_completion(completion);
  free_vio_pool(pool);
  pool = NULL;
}

/**
 * Most basic refCounts test.
 **/
static void testBasic(void)
{
  performAction(doTest, &completion);
}

/**********************************************************************/

static CU_TestInfo refCountsTests[] = {
  { "basic", testBasic },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo refCountsSuite = {
  .name        = "reference counter tests (RefCounts_t3)",
  .initializer = initializeRefCountsT3,
  .cleaner     = tearDownVDOTest,
  .tests       = refCountsTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &refCountsSuite;
}
