/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/bio.h>

#include "memory-alloc.h"
#include "uds-threads.h"

#include "data-vio.h"
#include "vdo.h"

#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  SECTOR_T_PER_SECTOR = VDO_SECTOR_SIZE / sizeof(sector_t),
};

static struct thread *oddThread;
static block_count_t  sectors;
static char          *data;

/**
 * Test-specific initialization.
 **/
static void initializePartialBlockWriteT1(void)
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
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(count * VDO_BLOCK_SIZE,
                                  char,
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
 * Write every other sector starting at a given sector.
 *
 * @param start           The sector at which to start
 * @param count           The number of sectors to write
 * @param expectedResult  The expected result of every write
 **/
static void doPartialWrites(sector_t start, sector_t count, int expectedResult)
{
  IORequest *requests[count];
  char *ptr = data + (VDO_SECTOR_SIZE * start);
  for (sector_t i = 0; i < count; i++) {
    requests[i] = launchUnalignedBufferBackedRequest((i * 2) + start,
                                                     1,
                                                     ptr,
                                                     REQ_OP_WRITE);
    ptr += (2 * VDO_SECTOR_SIZE);
  }

  for (block_count_t i = 0; i < count; i++) {
    CU_ASSERT_EQUAL(awaitAndFreeRequest(UDS_FORGET(requests[i])),
                    expectedResult);
  }
}

/**********************************************************************/
static void doOddWrites(void *arg __attribute__((unused)))
{
  doPartialWrites(1, sectors / 2, VDO_SUCCESS);
}

/**
 * Do partial writes in two threads, one writing even numbered blocks, one
 * odd.
 **/
static void testPartialWrites(void)
{
  populateBlockMapTree();
  block_count_t blocks = getPhysicalBlocksFree() / 2;
  /*
   * The total number of sectors available for concurrent writing. This is half
   * the total number of free sectors in order to avoid allocation issues.
   */
  sectors = blocks * VDO_SECTORS_PER_BLOCK;
  generateData(blocks);
  VDO_ASSERT_SUCCESS(uds_create_thread(doOddWrites,
                                       NULL,
                                       "oddWriter",
                                       &oddThread));
  doPartialWrites(0, sectors / 2, VDO_SUCCESS);
  VDO_ASSERT_SUCCESS(uds_join_threads(oddThread));

  char *buffer;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(blocks * VDO_BLOCK_SIZE,
                                  char,
                                  __func__,
                                  &buffer));
  VDO_ASSERT_SUCCESS(performRead(0, blocks, buffer));
  UDS_ASSERT_EQUAL_BYTES(data, buffer, blocks * VDO_BLOCK_SIZE);
  UDS_FREE(UDS_FORGET(buffer));
  UDS_FREE(UDS_FORGET(data));
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
static void testUnchangedSectorContents(void)
{
  generateData(1);
  setBIOSubmitHook(persistMetadataWrites);
  doPartialWrites(0, 1, VDO_SUCCESS);
  clearBIOSubmitHook();
  crashVDO();
  startVDO(VDO_DIRTY);

  char actualData[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(performRead(0, 1, actualData));
  // We wrote the 0th sector, so the other 7 sectors should be zeros.
  memset(data + VDO_SECTOR_SIZE, 0, VDO_SECTOR_SIZE * 7);
  UDS_ASSERT_EQUAL_BYTES(data, actualData, VDO_BLOCK_SIZE);
  UDS_FREE(UDS_FORGET(data));
}

/**********************************************************************/
static void testReadOnly(void)
{
  generateData(10);
  forceVDOReadOnlyMode();
  doPartialWrites(10, 10, VDO_READ_ONLY);

  stopVDO();
  startReadOnlyVDO(VDO_READ_ONLY_MODE);
  assertVDOState(VDO_READ_ONLY_MODE);
  doPartialWrites(20, 10, VDO_READ_ONLY);
  UDS_FREE(UDS_FORGET(data));
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test partial writes",                testPartialWrites           },
  { "test unchanged sector contents",     testUnchangedSectorContents },
  { "test partial I/O in read-only mode", testReadOnly                },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name                     = "partial write tests (PartialBlockWrites_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializePartialBlockWriteT1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
