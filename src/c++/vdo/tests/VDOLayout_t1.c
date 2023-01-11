/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "constants.h"
#include "slab.h"
#include "slab-summary.h"
#include "types.h"
#include "vdo.h"
#include "vdoConfig.h"
#include "vdo-layout.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"


static block_count_t      physicalSize = (1 << 20);
static block_count_t      slabSize     = (1 << 15);
static block_count_t      journalSize  = 8192;
static struct vdo_layout *vdoLayout;
static block_count_t      summarySize;

enum {
  LAYOUT_START = 5,
};

/**
 * Check that a partition has the given base, offset, and size.
 *
 * @param layout  The layout containing the partition
 * @param id      The partition id to check
 * @param base    The expected base
 * @param offset  The expected offset
 * @param size    The expected size
 **/
static void assertPartitionState(struct fixed_layout *layout,
                                 byte                 id,
                                 block_count_t        base,
                                 block_count_t        offset,
                                 block_count_t        size)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout, id, &partition));

  CU_ASSERT_EQUAL(base, vdo_get_fixed_layout_partition_base(partition));
  CU_ASSERT_EQUAL(offset, vdo_get_fixed_layout_partition_offset(partition));
  if (size != VDO_ALL_FREE_BLOCKS) {
    // Don't check the size of partitions expected to fill all free space.
    CU_ASSERT_EQUAL(size, vdo_get_fixed_layout_partition_size(partition));
  }
}

/**
 * Check that the layout was created as expected.
 **/
static void checkLayout(void)
{
  struct fixed_layout *layout = vdoLayout->layout;
  assertPartitionState(layout, VDO_BLOCK_MAP_PARTITION, 0, LAYOUT_START,
                       DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  assertPartitionState(layout, VDO_BLOCK_ALLOCATOR_PARTITION,
                       DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
                       DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT + LAYOUT_START,
                       VDO_ALL_FREE_BLOCKS);
  assertPartitionState(layout, VDO_RECOVERY_JOURNAL_PARTITION, 0,
                       (physicalSize - journalSize - summarySize),
                       journalSize);
  assertPartitionState(layout, VDO_SLAB_SUMMARY_PARTITION, 0,
                       physicalSize - summarySize, summarySize);
}

/**
 * Make a layout directly from test parameters and check that it is correct.
 **/
static void makeAndCheckLayout(void)
{
  struct fixed_layout *layout;
  int result
    = vdo_make_partitioned_fixed_layout(physicalSize, LAYOUT_START,
                                        DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
                                        journalSize, summarySize, &layout);
  VDO_ASSERT_SUCCESS(result);
  VDO_ASSERT_SUCCESS(vdo_decode_layout(layout, &vdoLayout));
  checkLayout();
}

/**********************************************************************/
static void creationTest(void)
{
  summarySize = vdo_get_slab_summary_size();
  struct vdo_config config = {
    .logical_blocks        = slabSize - 256 - 9,
    .physical_blocks       = physicalSize,
    .slab_size             = slabSize,
    .recovery_journal_size = journalSize,
    .slab_journal_blocks   = 224,
  };

  struct fixed_layout *layout;
  VDO_ASSERT_SUCCESS(makeFixedLayoutFromConfig(&config, LAYOUT_START,
                                               &layout));
  VDO_ASSERT_SUCCESS(vdo_decode_layout(layout, &vdoLayout));
  checkLayout();

  vdo_free_layout(UDS_FORGET(vdoLayout));

  makeAndCheckLayout();
  vdo_free_layout(UDS_FORGET(vdoLayout));
}

/**
 * Test that resizing a layout preserves the size and base of the
 * original partitions.
 **/
static void resizeTest(void)
{
  initializeDefaultBasicTest();
  summarySize = 93;
  makeAndCheckLayout();
  for (block_count_t newSize = physicalSize + 1; ; newSize++) {
    int result = prepare_to_vdo_grow_layout(vdoLayout, physicalSize, newSize);
    if (result == VDO_SUCCESS) {
      CU_ASSERT_EQUAL(newSize, vdo_grow_layout(vdoLayout));
      vdo_finish_layout_growth(vdoLayout);
      physicalSize = newSize;
      break;
    }

    CU_ASSERT_EQUAL(result, VDO_INCREMENT_TOO_SMALL);
  }

  checkLayout();
  vdo_free_layout(UDS_FORGET(vdoLayout));
  tearDownVDOTest();
}

/**********************************************************************/
static CU_TestInfo vdoLayoutTests[] = {
  { "creates partitions as expected", creationTest },
  { "resizes existing layout",        resizeTest   },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoLayoutSuite = {
  .name                     = "VDO layout tests (VDOLayout_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = vdoLayoutTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoLayoutSuite;
}
