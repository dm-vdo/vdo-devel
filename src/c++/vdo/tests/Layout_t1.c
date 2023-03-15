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
#include "dm-vdo-target.h"
#include "encodings.h"
#include "types.h"
#include "vdo.h"

#include "vdoConfig.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"


static block_count_t      physicalSize = (1 << 20);
static block_count_t      slabSize     = (1 << 15);
static block_count_t      journalSize  = 8192;
static block_count_t      checkedSize;

enum {
  LAYOUT_START = 5,
};

/**
 * Check that a partition has the given base, offset, and size.
 *
 * @param layout  The layout containing the partition
 * @param id      The partition id to check
 * @param offset  The expected offset
 * @param size    The expected size
 **/
static void
assertPartitionState(struct layout *layout, u8 id, block_count_t offset, block_count_t size)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_partition(layout, id, &partition));

  CU_ASSERT_EQUAL(offset, partition->offset);
  if (size != 0) {
    // Don't check the size of partitions expected to fill all free space.
    CU_ASSERT_EQUAL(size, partition->count);
  }

  checkedSize += partition->count;
}

/**
 * Check that the layout was created as expected.
 **/
static void checkLayout(struct layout *layout)
{
  checkedSize = layout->start;
  assertPartitionState(layout,
                       VDO_BLOCK_MAP_PARTITION,
                       LAYOUT_START,
                       DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  assertPartitionState(layout,
                       VDO_SLAB_DEPOT_PARTITION,
                       DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT + LAYOUT_START,
                       0);
  assertPartitionState(layout,
                       VDO_RECOVERY_JOURNAL_PARTITION,
                       (layout->size - journalSize - VDO_SLAB_SUMMARY_BLOCKS),
                       journalSize);
  assertPartitionState(layout,
                       VDO_SLAB_SUMMARY_PARTITION,
                       layout->size - VDO_SLAB_SUMMARY_BLOCKS,
                       VDO_SLAB_SUMMARY_BLOCKS);
  CU_ASSERT_EQUAL(layout->size, checkedSize);
}

/**********************************************************************/
static void testLayout(void)
{
  struct vdo_config config = {
    .logical_blocks        = slabSize - 256 - 9,
    .physical_blocks       = physicalSize,
    .slab_size             = slabSize,
    .recovery_journal_size = journalSize,
    .slab_journal_blocks   = 224,
  };

  struct vdo vdo;
  struct layout *layout = &vdo.layout;
  VDO_ASSERT_SUCCESS(vdo_initialize_layout(physicalSize,
                                           LAYOUT_START,
                                           DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT,
                                           journalSize,
                                           VDO_SLAB_SUMMARY_BLOCKS,
                                           layout));
  checkLayout(layout);
  vdo_uninitialize_layout(layout);

  VDO_ASSERT_SUCCESS(initializeLayoutFromConfig(&config, LAYOUT_START, layout));
  checkLayout(layout);

  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(uds_make_buffer(VDO_LAYOUT_ENCODED_SIZE, &buffer));
  VDO_ASSERT_SUCCESS(encode_layout(layout, buffer));
  vdo_uninitialize_layout(layout);

  VDO_ASSERT_SUCCESS(decode_layout(buffer, LAYOUT_START, physicalSize, layout));
  uds_free_buffer(UDS_FORGET(buffer));
  checkLayout(layout);

  memset(&vdo.next_layout, 0, sizeof(struct layout));
  vdo.partition_copier = NULL;
  for (block_count_t newSize = physicalSize + 1; ; newSize++) {
    int result = grow_layout(&vdo, physicalSize, newSize);
    if (result == VDO_SUCCESS) {
      checkLayout(&vdo.next_layout);
      break;
    }

    CU_ASSERT_EQUAL(result, VDO_INCREMENT_TOO_SMALL);
  }

  vdo_uninitialize_layout(&vdo.next_layout);
  vdo_uninitialize_layout(layout);
  dm_kcopyd_client_destroy(UDS_FORGET(vdo.partition_copier));
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "test layout", testLayout },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Layout tests (Layout_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
