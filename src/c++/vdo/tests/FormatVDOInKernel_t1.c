/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "constants.h"
#include "fileUtils.h"
#include "logger.h"

#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // Must be large enough to have enough logical space to span all tree roots.
  PHYSICAL_BLOCKS = DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT * 1024 * 2,
};

/**********************************************************************/
TestParameters testParameters = {
  .indexMemory        = UDS_MEMORY_CONFIG_256MB,
  .journalBlocks      = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
  .slabJournalBlocks  = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
  .slabSize           = 512,
  .formatInKernel     = true,
};

/**********************************************************************/
static void minimumVDOTest(void)
{

  testParameters.slabCount = 1;
  testParameters.physicalBlocks
    = 1 + 1 + 60 + 512 + DEFAULT_VDO_RECOVERY_JOURNAL_SIZE + VDO_SLAB_SUMMARY_BLOCKS;
  initializeVDOTest(&testParameters);

  VDO_ASSERT_SUCCESS(vdo_validate_component_states(&vdo->states,
                                                   vdo->geometry.nonce,
                                                   vdo->device_config->physical_blocks,
                                                   vdo->device_config->logical_blocks));
}

/**********************************************************************/
static void assertPartitionIsZeroed(struct vdo *vdo, enum partition_id id)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_partition(&vdo->states.layout, id, &partition));

  char zeroBlock[VDO_BLOCK_SIZE];
  memset(zeroBlock, 0, VDO_BLOCK_SIZE);

  off_t offset = (off_t)(partition->offset * VDO_BLOCK_SIZE);

  char buffer[VDO_BLOCK_SIZE];
  size_t read;
  for (block_count_t i = 0; i < partition->count; i++) {
    VDO_ASSERT_SUCCESS(read_data_at_offset(vdo->device_config->owned_device->bdev->fd,
                                           offset + (i * VDO_BLOCK_SIZE), buffer,
                                           VDO_BLOCK_SIZE, &read));
    UDS_ASSERT_EQUAL_BYTES(buffer, zeroBlock, VDO_BLOCK_SIZE);
  }
}

/**********************************************************************/
static void zeroingTest(void)
{
  initializeVDOTest(&testParameters);
  assertPartitionIsZeroed(vdo, VDO_BLOCK_MAP_PARTITION);
  assertPartitionIsZeroed(vdo, VDO_RECOVERY_JOURNAL_PARTITION);
}

static CU_TestInfo tests[] = {
  { "Zeroes expected partitions", zeroingTest },
  { "Minimum VDO Size Test", minimumVDOTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO format in kernel tests (FormatVDOInKernel_t1)",
  .initializerWithArguments = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
