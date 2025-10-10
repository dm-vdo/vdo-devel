/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "albtest.h"

#include <linux/bio.h>

#include "memory-alloc.h"

#include "data-vio.h"
#include "vdo.h"

#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  SECTOR_T_PER_SECTOR = VDO_SECTOR_SIZE / sizeof(sector_t),
  DATA_BLOCKS = 4,
};

static char *data;
static char *expectedData;
static char *actualData;

/**
 * Test-specific initialization.
 **/
static void initializePartialDiscardsT1(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 64,
    .journalBlocks  = 8,
  };

  initializeVDOTest(&parameters);
}

/**
 * Allocate a buffer representing all of the writes we intend to do. Fill each
 * sector with its sector number + 1 (we don't want to start at zero as we
 * don't want the first sector to be zero-eliminated).
 *
 * @param count  The number of blocks of data to generate
 **/
static void generateData(block_count_t count)
{
  VDO_ASSERT_SUCCESS(vdo_allocate(count * VDO_BLOCK_SIZE,
                                  __func__,
                                  &data));

  sector_t *ptr = (sector_t *) data;
  for (sector_t i = 0; i < (count * VDO_SECTORS_PER_BLOCK); i++) {
    for (uint32_t j = 0; j < SECTOR_T_PER_SECTOR; j++, ptr++) {
      *ptr = i + 1;
    }
  }
}

/**
 * Make sure all metadata writes are immediately persisted.
 *
 * Implements BIOSubmitHook.
 **/
static bool persistMetadataWrites(struct bio *bio)
{
  if ((bio_op(bio) == REQ_OP_WRITE)
      && (bio->bi_vcnt > 0)
      && !is_data_vio(bio->bi_private)) {
    bio->bi_opf |= REQ_FUA;
  }

  return true;
}

/**********************************************************************/
static void testUnalignedDiscards(void)
{
  generateData(DATA_BLOCKS);
  VDO_ASSERT_SUCCESS(vdo_allocate(DATA_BLOCKS * VDO_BLOCK_SIZE, "expected data",
                                  &expectedData));
  VDO_ASSERT_SUCCESS(vdo_allocate(DATA_BLOCKS * VDO_BLOCK_SIZE, "actual data",
                                  &actualData));
  setBIOSubmitHook(persistMetadataWrites);

  /** Try odd-sized discards at each offset */
  for (int start = 1; start < VDO_SECTORS_PER_BLOCK; start++) {
    for (int length = 4; length < 24; length += 4) {
      VDO_ASSERT_SUCCESS(performWrite(0, DATA_BLOCKS, data));

      memcpy(expectedData, data, DATA_BLOCKS * VDO_BLOCK_SIZE);
      memset(expectedData + (start * VDO_SECTOR_SIZE), 0,
             length * VDO_SECTOR_SIZE);

      IORequest *request = launchUnalignedTrim(start, length);
      VDO_ASSERT_SUCCESS(awaitAndFreeRequest(vdo_forget(request)));

      VDO_ASSERT_SUCCESS(performRead(0, DATA_BLOCKS, actualData));
      UDS_ASSERT_EQUAL_BYTES(expectedData, actualData,
                            DATA_BLOCKS * VDO_BLOCK_SIZE);

      crashVDO();
      startVDO(VDO_DIRTY);

      VDO_ASSERT_SUCCESS(performRead(0, DATA_BLOCKS, actualData));
      UDS_ASSERT_EQUAL_BYTES(expectedData, actualData,
                             DATA_BLOCKS * VDO_BLOCK_SIZE);
    }
  }
  
  /* Try the same thing with an initial zero block. */
   for (int start = 1; start < VDO_SECTORS_PER_BLOCK; start++) {
    for (int length = 4; length < 24; length += 4) {
      zeroData(0, 1, VDO_SUCCESS);
      VDO_ASSERT_SUCCESS(performWrite(1, DATA_BLOCKS - 1, data));

      int zeroSectors = max(start + length, 8);
      memcpy(expectedData + VDO_BLOCK_SIZE, data,
             (DATA_BLOCKS - 1) * VDO_BLOCK_SIZE);
      memset(expectedData, 0, zeroSectors * VDO_SECTOR_SIZE);

      IORequest *request = launchUnalignedTrim(start, length);
      VDO_ASSERT_SUCCESS(awaitAndFreeRequest(vdo_forget(request)));

      VDO_ASSERT_SUCCESS(performRead(0, DATA_BLOCKS, actualData));
      UDS_ASSERT_EQUAL_BYTES(expectedData, actualData,
                             DATA_BLOCKS * VDO_BLOCK_SIZE);

      crashVDO();
      startVDO(VDO_DIRTY);

      VDO_ASSERT_SUCCESS(performRead(0, DATA_BLOCKS, actualData));
      UDS_ASSERT_EQUAL_BYTES(expectedData, actualData,
                             DATA_BLOCKS * VDO_BLOCK_SIZE);
    }
  }

  clearBIOSubmitHook();
  vdo_free(vdo_forget(data));
  vdo_free(vdo_forget(actualData));
  vdo_free(vdo_forget(expectedData));
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test unaligned discards", testUnalignedDiscards },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "partial discard tests (PartialDiscards_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializePartialDiscardsT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
