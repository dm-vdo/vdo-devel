/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "constants.h"

#include "dataBlocks.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  DATA_BLOCKS = 256,
};

static block_count_t freeBlocks;

/**
 * Test-specific initialization.
 **/
static void initializeDiscard_t1(void)
{
  const TestParameters parameters = {
    .mappableBlocks = 1024,
    .journalBlocks  = 16,
    .dataFormatter  = fillWithOffsetPlusOne,
  };
  initializeVDOTest(&parameters);
  freeBlocks = populateBlockMapTree();
}

/**
 * Test writing some blocks and trimming them away.
 **/
static void testDiscard(void)
{
  // Write the data set.
  writeAndVerifyData(0, 0, DATA_BLOCKS, freeBlocks - DATA_BLOCKS, DATA_BLOCKS);

  // Trim the data.
  trimAndVerifyData(0, DATA_BLOCKS, freeBlocks, 0);
}

/**
 * Write blocks of duplicated data then trim it away.
 **/
static void testDiscardDuplicateBlocks(void)
{
  // Write the data set.
  writeAndVerifyData(0, 0, DATA_BLOCKS, freeBlocks - DATA_BLOCKS, DATA_BLOCKS);

  // Append a new copy.
  writeAndVerifyData(DATA_BLOCKS, 0, DATA_BLOCKS, freeBlocks - DATA_BLOCKS,
                     DATA_BLOCKS);

  // Trim the data.
  trimAndVerifyData(0, DATA_BLOCKS, freeBlocks - DATA_BLOCKS, DATA_BLOCKS);

  // Verify second copy.
  verifyData(DATA_BLOCKS, 0, DATA_BLOCKS);

  // Trim the second copy.
  trimAndVerifyData(DATA_BLOCKS, DATA_BLOCKS, freeBlocks, 0);
}

/**
 * Write data and trim the middle of it.
 **/
static void testDiscardWithHoles(void)
{
  // Write the data set.
  writeAndVerifyData(0, 0, DATA_BLOCKS, freeBlocks - DATA_BLOCKS, DATA_BLOCKS);

  const block_count_t holeStart = DATA_BLOCKS / 4;
  const block_count_t holeEnd   = 3 * DATA_BLOCKS / 4;

  // Trim the middle data.
  trimAndVerifyData(holeStart, holeEnd - holeStart,
                    freeBlocks - (DATA_BLOCKS / 2), DATA_BLOCKS / 2);

  // Verify existing data.
  verifyData(0, 0, holeStart);
  verifyData(holeEnd, holeEnd, DATA_BLOCKS - holeEnd);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test simple discard",      testDiscard                },
  { "test duplicate discard",   testDiscardDuplicateBlocks },
  { "test discard with holes",  testDiscardWithHoles       },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name                     = "Discard_t1",
  .initializer              = initializeDiscard_t1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
