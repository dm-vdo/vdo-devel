/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <unistd.h>
#include <sys/types.h>

#include "testUtils.h"

#include "fileUtils.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "string-utils.h"
#include "syscalls.h"

#include "encodings.h"

#include "vdoConfig.h"

#include "asyncLayer.h"
#include "ioRequest.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum { INDEX_BLOCKS = 0 };

/**
 * Fill a data block with repetitions of the 64-bit index of the data block.
 * This is like fillWithOffset, but always uses little endian byte order so
 * the pickled data blocks will also be platform-independent.
 * Implements DataFormatter.
 **/
static void fillWithLittleEndianIndex(char *block, block_count_t index)
{
  for (size_t offset = 0;
       offset < VDO_BLOCK_SIZE;
       offset += sizeof(uint64_t)) {
         put_unaligned_le64(index, (u8 *) &block[offset]);
  }
}

static const TestParameters PARAMETERS = {
  .mappableBlocks = 64,
  // This needs to match the specified config in Upgrade_t1.
  .logicalBlocks  = 128,
  .journalBlocks  = 2,
  .slabSize       = 128,
  .dataFormatter  = fillWithLittleEndianIndex,
  .noIndexRegion  = true,
};

static const nonce_t  NONCE     = 0xdeadbeefacedfeed;
static       uuid_t   TEST_UUID = "flying VDO @ RH";

static char *CURRENT_VERSION_FILE_NAME = "testdata/vdo.current";
static char *currentVersionFileName    = NULL;
static char *pickledData               = NULL;

/**
 * Make the full path name of the current version VDO data file.
 *
 * @param name  The relative name of the file (relative to the test directory)
 **/
static void makeFileName(const char *name)
{
  if (*name == '/') {
    VDO_ASSERT_SUCCESS(uds_alloc_sprintf("current version file name",
                                         &currentVersionFileName, "%s", name));
    return;
  }

  VDO_ASSERT_SUCCESS(uds_alloc_sprintf("current version file name",
                                       &currentVersionFileName, "%s/%s",
                                       getTestDirectory(), name));
}

/**
 * Make a VDO with deterministic contents.
 **/
static void prepareVDO(void)
{
  stopVDO();
  struct vdo_config config = getTestConfig().config;
  VDO_ASSERT_SUCCESS(formatVDOWithNonce(&config, NULL, getSynchronousLayer(),
                                        NONCE, &TEST_UUID));
  startVDO(VDO_NEW);

  /*
   * Write some data, one block at a time with no dedupe or
   * compression so that the contents of the journal, block map, and
   * reference tracker are deterministic. This will hopefully allow us
   * to detect changes which should result in a version number change.
   */
  for (unsigned int i = 0; i < 48; i++) {
    writeData(1 + i, 0 + i, 1, VDO_SUCCESS);
  }
  stopVDO();
}

/**
 * Make a VDO with deterministic contents and then write it to a file.
 * This function uses the RAM layer instead of the file layer in order
 * to ensure that unwritten portions of the VDO are zeroed out instead of
 * being random.
 **/
static void pickleVDO(void)
{
  int fd;
  VDO_ASSERT_SUCCESS(open_file(currentVersionFileName, FU_CREATE_WRITE_ONLY,
                               &fd));
  dumpRAMLayerToFile(getSynchronousLayer(), fd);
  close(fd);
}

/**********************************************************************/
static void initializeVDOVersionT1(int argc, const char **argv)
{
  makeFileName((argc > 1) ? argv[1] : CURRENT_VERSION_FILE_NAME);
  if (argc > 0) {
    initializeVDOTest(&PARAMETERS);
    prepareVDO();
    pickleVDO();
    argc = 0;
    tearDownVDOTest();
  }

  initializeVDOTest(&PARAMETERS);
}

/**********************************************************************/
static void tearDownVDOVersionT1(void)
{
  vdo_free(pickledData);
  tearDownVDOTest();
  vdo_free(currentVersionFileName);
}

/**********************************************************************/
static void readVDOFromDisk(const char *fileName)
{
  int fd;
  VDO_ASSERT_SUCCESS(open_file(fileName, FU_READ_ONLY, &fd));

  off_t vdoSize;
  VDO_ASSERT_SUCCESS(get_open_file_size(fd, &vdoSize));

  VDO_ASSERT_SUCCESS(vdo_allocate(vdoSize, char, __func__, &pickledData));
  ssize_t bytesRead;
  VDO_ASSERT_SUCCESS(logging_read(fd, pickledData, vdoSize, __func__,
                                  &bytesRead));
  CU_ASSERT_EQUAL(vdoSize, bytesRead);
  close(fd);
}

/**
 * Check whether a given pbn is an acceptable mismatch.
 *
 * Implements MismatchChecker.
 **/
static void mismatchChecker(physical_block_number_t pbn,
                            char *expectedBlock,
                            char *actualBlock)
{
  if (pbn == 320) {
    /*
     * The slab summary entry for slab 0 has two valid states. Depending on
     * timing of I/Os, the draining ref counts and draining slab journal can
     * make their slab journal updates in either order. Hence it is valid
     * for slab 0 to be dirty or clean.
     */
    size_t entrySize = sizeof(struct slab_summary_entry);
    struct slab_summary_entry entry
      = *((struct slab_summary_entry *) actualBlock);
    CU_ASSERT_EQUAL(entry.tail_block_offset, 0);
    CU_ASSERT_EQUAL(entry.fullness_hint, 6);
    CU_ASSERT_EQUAL(entry.load_ref_counts, 1);
    // We don't check the dirty bit as it could be set either way.
    UDS_ASSERT_EQUAL_BYTES(expectedBlock + entrySize,
                           actualBlock + entrySize,
                           VDO_BLOCK_SIZE - entrySize);
    return;
  }

  // At this time, there should be no other mismatches */
  CU_FAIL("Unexpected mismatch at pbn %" PRIu64, pbn);
}

/**
 * Check that the on-disk format of a VDO has not changed since the last time
 * we bumped the volume version number.
 *
 * If this test if failing due to the current version no longer being current,
 * a new pickled VDO can be generated by running:
 *
 * ./albtest VDOVersion_t1 -- --pickle
 **/
static void testCurrentVersion(void)
{
  readVDOFromDisk(currentVersionFileName);
  prepareVDO();
  checkRAMLayerContents(getSynchronousLayer(), pickledData, mismatchChecker);
}

static CU_TestInfo tests[] = {
  { "Test current on disk format has not changed", testCurrentVersion },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "VDO Version T1 (VDOVersion_t1)",
  .initializerWithArguments = initializeVDOVersionT1,
  .initializer              = NULL,
  .cleaner                  = tearDownVDOVersionT1,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
