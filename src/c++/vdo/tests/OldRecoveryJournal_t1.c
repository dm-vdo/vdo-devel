/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/bio.h>

#include "testUtils.h"

#include "fileUtils.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "syscalls.h"

#include "constants.h"
#include "packer.h"
#include "statistics.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"

#include "vdoConfig.h"

#include "asyncLayer.h"
#include "ioRequest.h"
#include "ramLayer.h"
#include "vdoTestBase.h"

enum {
  BATCH_SIZE = VDO_MAX_COMPRESSION_SLOTS * 2,
  BATCHES = (RECOVERY_JOURNAL_1_ENTRIES_PER_BLOCK / BATCH_SIZE) + 1,
};

static TestParameters parameters = {
  .mappableBlocks      = 64,
  .logicalBlocks       = VDO_BLOCK_MAP_ENTRIES_PER_PAGE * 2,
  .journalBlocks       = 16,
  .logicalThreadCount  = 3,
  .physicalThreadCount = 2,
  .hashZoneThreadCount = 2,
  .dataFormatter       = fillWithOffsetPlusOne,
};

static const char *CRASHED   = "testdata/vdo.old.rj.crashed";
static const char *RECOVERED = "testdata/vdo.old.rj.recovered";
static char *crashedPath;
static char *recoveredPath;

static bool generateFiles = false;

/**********************************************************************/
static void initialize(int argc, const char **argv __attribute__((unused)))
{
  generateFiles = (argc > 0);

  VDO_ASSERT_SUCCESS(vdo_alloc_sprintf("crashed file name",
                                       &crashedPath,
                                       "%s/%s",
                                       getTestDirectory(),
                                       CRASHED));
  VDO_ASSERT_SUCCESS(vdo_alloc_sprintf("recovered file name",
                                       &recoveredPath,
                                       "%s/%s",
                                       getTestDirectory(),
                                       RECOVERED));
}

/**********************************************************************/
static void cleanUp(void)
{
  vdo_free(crashedPath);
  vdo_free(recoveredPath);
}

/**********************************************************************/
static bool checkJournalFormat(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
    CU_ASSERT_EQUAL(VDO_METADATA_RECOVERY_JOURNAL,
                    ((struct packed_journal_header *) vio->data)->metadata_type);
    clearBIOSubmitHook();
  }

  return true;
}

/**
 * Generate the pickles used to test upgrading from the old format. This will
 * do nothing unless this test is invoked with an argument (./vdotest
 * OldRecoveryJournal_t1 -- foo). It will fail and not update the pickles if
 * run in a tree which uses the new recovery journal format. It is mostly here
 * to preserve the history of how the pickles were generated and to make the
 * other test cases easier to understand.
 **/
static void generate(void)
{
  if (!generateFiles) {
    return;
  }

  initializeVDOTest(&parameters);

  // Don't generate files unless we are using the old journal format.
  setBIOSubmitHook(checkJournalFormat);

  // fill four recovery journal blocks, each batch would fill one compressed block.
  for (u8 i = 0; i < BATCHES; i++) {
    writeData(i * BATCH_SIZE, 0, BATCH_SIZE, VDO_SUCCESS);
  }

  // Overwrite one batch with zeros.
  zeroData(0, BATCH_SIZE, VDO_SUCCESS);

  // fill two more journal blocks with duplicates of a compressed block.
  modifyCompressDedupe(true, true);
  for (u8 i = 0; i < BATCHES; i++) {
    writeData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE + (i * BATCH_SIZE),
              BATCH_SIZE,
              BATCH_SIZE,
              VDO_SUCCESS);
  }

  // Discard one batch.
  discardData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE, BATCH_SIZE, VDO_SUCCESS);

  crashVDO();
  int fd;
  VDO_ASSERT_SUCCESS(open_file(crashedPath, FU_CREATE_WRITE_ONLY, &fd));
  dumpRAMLayerToFile(getSynchronousLayer(), fd);
  close(fd);
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  stopVDO();
  VDO_ASSERT_SUCCESS(open_file(recoveredPath, FU_CREATE_WRITE_ONLY, &fd));
  dumpRAMLayerToFile(getSynchronousLayer(), fd);
  close(fd);
}

/**
 * Read the VDO contents from disk and store in a char array.
 * 
 * The allocated array must be freed by the calling function.
 **/
static void saveExpectedFileContents(const char *fileName, char **expectedData)
{
  int fd;
  off_t vdoSize;
  ssize_t bytesRead;
  
  VDO_ASSERT_SUCCESS(open_file(fileName, FU_READ_ONLY, &fd));
  VDO_ASSERT_SUCCESS(get_open_file_size(fd, &vdoSize));
  VDO_ASSERT_SUCCESS(vdo_allocate(vdoSize, char, __func__, expectedData));
  VDO_ASSERT_SUCCESS(logging_read(fd, *expectedData, vdoSize, __func__,
                                  &bytesRead));
  CU_ASSERT_EQUAL(vdoSize, bytesRead);
  close(fd);
}

/**
 * Error if a mismatch is detected for a given pbn, as there are no
 * acceptable mismatches in this implementation.
 *
 * Implements MismatchChecker.
 **/
static void mismatchChecker(physical_block_number_t pbn,
                            char *expectedBlock,
                            char *actualBlock)
{
  CU_FAIL("Unexpected mismatch at pbn %" PRIu64 ": expected '%s', actual '%s'",
          pbn, expectedBlock, actualBlock);
}

/**********************************************************************/
static void verify(u64 recoveryCount)
{
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.logical_blocks_used, ((BATCHES * 2) - 1) * BATCH_SIZE);
  CU_ASSERT_EQUAL(stats.data_blocks_used, BATCH_SIZE + 2);
  CU_ASSERT_EQUAL(stats.read_only_recoveries, recoveryCount);

  for (page_number_t page = 0; page < 2; page++) {
    logical_block_number_t offset = page * VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

    verifyZeros(offset, BATCH_SIZE);

    for (u8 i = 1; i < BATCHES; i++) {
      verifyData(offset + (i * BATCH_SIZE), BATCH_SIZE * page, BATCH_SIZE);
    }

    verifyZeros(offset + (BATCHES * BATCH_SIZE),
                VDO_BLOCK_MAP_ENTRIES_PER_PAGE - BATCHES * BATCH_SIZE);
  }
}

/**********************************************************************/
static void testClean(void)
{
  parameters.backingFile = recoveredPath;
  initializeVDOTest(&parameters);
  verify(0);
  tearDownVDOTest();
}

/**********************************************************************/
static void testDirty(void)
{
  char *expectedData = NULL;
  
  parameters.backingFile = crashedPath;
  initializeTest(&parameters);
  saveExpectedFileContents(crashedPath, &expectedData);

  // VDO_UNSUPPORTED_VERSION is translated to -EINVAL upon load failure
  startVDOExpectError(-EINVAL);

  stopVDO();
  checkRAMLayerContents(getSynchronousLayer(), expectedData, mismatchChecker);
  vdo_free(expectedData);
  tearDownVDOTest();
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "generate test files (no-op without an argument)",      generate  },
  { "test loading a clean VDO with the old journal format", testClean },
  { "test loading a dirty VDO with the old journal format", testDirty },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Old recovery journal format (OldRecoveryJournal_t1)",
  .initializerWithArguments = initialize,
  .initializer              = NULL,
  .cleaner                  = cleanUp,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
