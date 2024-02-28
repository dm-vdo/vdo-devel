/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/dm-kcopyd.h>

#include "memory-alloc.h"
#include "syscalls.h"

#include "constants.h"
#include "types.h"

#include "physicalLayer.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  STRIDE     = 2048,
};

struct dm_kcopyd_client *copier;
block_count_t sectors;

static void dmKcopydCallback(int readResult, unsigned long writeResult, void *context)
{
  struct vdo_completion *completion = context;
  int result = ((readResult == 0) && (writeResult == 0)) ? VDO_SUCCESS : -EIO;
  vdo_fail_completion(completion, result);
}

/**********************************************************************/
static void dmKcopydAction(struct vdo_completion *completion)
{
  struct dm_io_region from = (struct dm_io_region) {
    .sector = 0,
    .count  = sectors,
  };
  struct dm_io_region to[1];
  to[0] = (struct dm_io_region) {
    .sector = sectors,
    .count  = sectors,
  };

  dm_kcopyd_copy(copier, &from, 1, to, 0, dmKcopydCallback, completion);
}

/**
 * Test copying a partition to another partition on the same layer.
 *
 * @param regionSize
 **/
static void testDmKcopyd(block_count_t regionSize)
{
  // The underlying layer must have space for a super block as well.
  block_count_t totalSize       = (2 * regionSize);
  TestParameters testParameters = {
    .physicalBlocks = totalSize,
    .slabSize       = 16, // Required when setting physicalBlocks
    .noIndexRegion  = true,
  };
  initializeBasicTest(&testParameters);

  // Generate data.
  char *data;
  UDS_ASSERT_SUCCESS(vdo_allocate(VDO_BLOCK_SIZE * totalSize, char,
                                  "test data", &data));
  for (block_count_t i = 0; i < totalSize; i++) {
    memset(&data[i * VDO_BLOCK_SIZE], i, VDO_BLOCK_SIZE);
  }

  // Fill every (non-zero) physical block with data.
  layer->writer(layer, 0, totalSize, data);

  // Setup is finished. Now, for the copy.
  copier = dm_kcopyd_client_create(NULL);
  sectors = regionSize * VDO_SECTORS_PER_BLOCK;
  performSuccessfulAction(dmKcopydAction);
  dm_kcopyd_client_destroy(copier);

  // Verify that the original data has not been touched.
  char *buffer;
  UDS_ASSERT_SUCCESS(vdo_allocate(VDO_BLOCK_SIZE * regionSize, char,
                                  "verification buffer", &buffer));
  VDO_ASSERT_SUCCESS(layer->reader(layer, 0, regionSize, buffer));
  UDS_ASSERT_EQUAL_BYTES(buffer, data, VDO_BLOCK_SIZE * regionSize);

  // Verify that the destination region is the same as the source now.
  VDO_ASSERT_SUCCESS(layer->reader(layer, regionSize, regionSize,
                                   buffer));
  UDS_ASSERT_EQUAL_BYTES(buffer, data, VDO_BLOCK_SIZE * regionSize);

  vdo_free(buffer);
  vdo_free(data);
  tearDownVDOTest();
}

/**********************************************************************/
static void testDmKcopydSmall(void)
{
  testDmKcopyd(STRIDE / 2);
}

/**********************************************************************/
static void testDmKcopydOneStride(void)
{
  testDmKcopyd(STRIDE);
}

/**********************************************************************/
static void testDmKcopydMultipleStrides(void)
{
  testDmKcopyd(STRIDE * 3);
}

/**********************************************************************/
static void testDmKcopydLargeNonAligned(void)
{
  testDmKcopyd(STRIDE * 5 / 2);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "small region copy",           testDmKcopydSmall           },
  { "one-stride region copy",      testDmKcopydOneStride       },
  { "many-stride region copy",     testDmKcopydMultipleStrides },
  { "unaligned large region copy", testDmKcopydLargeNonAligned },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name  = "dm-kcopyd fake tests (DmKcopydFake_t1)",
  .tests = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
