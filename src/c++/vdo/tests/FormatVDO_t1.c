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

#include "userVDO.h"
#include "vdoConfig.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void assertPartitionIsZeroed(UserVDO *vdo, enum partition_id id)
{
  struct partition *partition;
  VDO_ASSERT_SUCCESS(vdo_get_partition(&vdo->states.layout, id, &partition));

  char zeroBlock[VDO_BLOCK_SIZE];
  memset(zeroBlock, 0, VDO_BLOCK_SIZE);

  char buffer[VDO_BLOCK_SIZE];
  for (block_count_t i = 0; i < partition->count; i++) {
    VDO_ASSERT_SUCCESS(vdo->layer->reader(vdo->layer, partition->offset + i, 1, buffer));
    UDS_ASSERT_EQUAL_BYTES(buffer, zeroBlock, VDO_BLOCK_SIZE);
  }
}

/**********************************************************************/
static void zeroingTest(void)
{
  struct vdo_config config = getTestConfig().config;
  VDO_ASSERT_SUCCESS(formatVDO(&config, NULL, getSynchronousLayer()));
  UserVDO *vdo;
  VDO_ASSERT_SUCCESS(loadVDO(getSynchronousLayer(), true, &vdo));
  assertPartitionIsZeroed(vdo, VDO_BLOCK_MAP_PARTITION);
  assertPartitionIsZeroed(vdo, VDO_RECOVERY_JOURNAL_PARTITION);
  freeUserVDO(&vdo);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "Zeroes expected partitions", zeroingTest },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO format tests (FormatVDO_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDefaultBasicTest,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
