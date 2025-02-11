 /*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "syscalls.h"

#include "constants.h"
#include "encodings.h"
#include "vdo.h"

#include "fileUtils.h"
#include "userVDO.h"
#include "vdoConfig.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void assertPartitionIsZeroed(struct vdo_component_states *states,
                                    PhysicalLayer *layer,
                                    enum partition_id id)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_partition(&states->layout, id, &partition));

  char zeroBlock[VDO_BLOCK_SIZE];
  memset(zeroBlock, 0, VDO_BLOCK_SIZE);

  char buffer[VDO_BLOCK_SIZE];
  for (block_count_t i = 0; i < partition->count; i++) {
    VDO_ASSERT_SUCCESS(layer->reader(layer, partition->offset + i, 1, buffer));
    UDS_ASSERT_EQUAL_BYTES(buffer, zeroBlock, VDO_BLOCK_SIZE);
  }
}

/**********************************************************************/
static void zeroingTest(void)
{
  initializeTest(NULL);

  block_count_t minVDOBlocks;
  TestConfiguration config = getTestConfig();
  VDO_ASSERT_SUCCESS(calculateMinimumVDOFromConfig(&config.config,
                                                   &config.indexConfig,
                                                   &minVDOBlocks));
  VDO_ASSERT_SUCCESS(formatVDO(&config.config, NULL, getSynchronousLayer()));
  UserVDO *vdo;
  VDO_ASSERT_SUCCESS(loadVDO(getSynchronousLayer(), true, &vdo));
  assertPartitionIsZeroed(&vdo->states, vdo->layer, VDO_BLOCK_MAP_PARTITION);
  assertPartitionIsZeroed(&vdo->states, vdo->layer, VDO_RECOVERY_JOURNAL_PARTITION);
  freeUserVDO(&vdo);
}

/**********************************************************************/
static void assertKernelPartitionIsZeroed(struct vdo *vdo,
                                           enum partition_id id)
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
static void zeroingInKernelTest(void)
{
  // These default values are taken from vdoFormat.c
  TestParameters testParameters = {
    .indexMemory        = UDS_MEMORY_CONFIG_256MB,
    .journalBlocks      = DEFAULT_VDO_RECOVERY_JOURNAL_SIZE,
    .slabJournalBlocks  = DEFAULT_VDO_SLAB_JOURNAL_SIZE,
    .slabSize           = 512,
    .formatInKernel     = true,
  };

  // Test format in kernel normally
  initializeVDOTest(&testParameters);

  assertKernelPartitionIsZeroed(vdo, VDO_BLOCK_MAP_PARTITION);
  assertKernelPartitionIsZeroed(vdo, VDO_RECOVERY_JOURNAL_PARTITION);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Zeroes expected partitions", zeroingTest },
  { "Zeroes expected partitions (kernel formatting)", zeroingInKernelTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO format tests (FormatVDO_t1)",
  .initializerWithArguments = NULL,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
