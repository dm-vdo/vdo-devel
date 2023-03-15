/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "block-map.h"
#include "encodings.h"
#include "slab-depot.h"

#include "blockMapUtils.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  BLOCK_COUNT = 263,
};

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  TestParameters parameters = {
    .dataFormatter  = fillWithOffsetPlusOne,
    .logicalBlocks  = BLOCK_COUNT,
    .journalBlocks  = 32,
    .slabCount      = 1,
    .slabSize       = 8,
    // Geometry + super block + root count + one slab + recovery journal
    // + slab summary
    .physicalBlocks = 1 + 1 + 60 + 8 + 32 + 64,
    .synchronousStorage = true,
  };

  initializeVDOTest(&parameters);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 4);
  populateBlockMapTree();
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 0);

  restartVDO(false);
  // We only need one block, but we have to grow by at least journal + summary
  // so we grow by 12 slabs.
  addSlabs(12);

  initializeBlockMapUtils(BLOCK_COUNT);
  restartVDO(false);
}

/**
 * Test-specific teardown.
 **/
static void teardown(void)
{
  tearDownVDOTest();
  tearDownBlockMapUtils();
}

/**
 * Implements PopulateBlockMapConfigurator.
 **/
static void configureCorruptBlocks(struct data_vio *dataVIO)
{
  logical_block_number_t   lbn   = dataVIO->logical.lbn;
  enum block_mapping_state state = VDO_MAPPING_STATE_UNCOMPRESSED;
  physical_block_number_t  pbn;

  switch (lbn) {
  // The first few LBNs will map to various out of range places.
  case 0:
    pbn = 1;
    break;

  case 1:
    pbn = getTestConfig().config.physical_blocks + 1;
    break;

  case 2:
    pbn = getTestConfig().config.physical_blocks - 1;
    break;

  case 3:
    pbn = vdo->depot->slabs[1]->start - 1;
    break;

  case 4:
    // An LBN which maps to a block map page (the whole first slab is block
    // map).
    pbn = vdo->depot->slabs[0]->start;
    break;

  case 5:
    // An LBN which is unmapped, but nevertheless has a non-zero PBN
    pbn = vdo->depot->slabs[1]->start + 1;
    state = VDO_MAPPING_STATE_UNMAPPED;
    break;

  case 6:
    // An LBN which is compressed, but has no PBN
    pbn = 0;
    state = VDO_MAPPING_STATE_COMPRESSED_MAX;
    break;

  default:
    // The final 256 lbns will all be mapped to the same pbn. On rebuild, two
    // of them will be removed, so we only set expectations for the first 254.
    pbn = vdo->depot->slabs[1]->start;
    if (lbn < (MAXIMUM_REFERENCE_COUNT + 7)) {
      setBlockMapping(lbn, pbn, state);
    }
  }

  dataVIO->recovery_sequence_number = 1;
  dataVIO->new_mapped.pbn           = pbn;
  dataVIO->new_mapped.state         = state;
}

/**
 * Verify that bad references in the leaf pages are removed during read only
 * rebuild. A tree is constructed with leaf pages pointing at various wrong
 * addresses; reference count rebuild will remove those invalid mappings.
 **/
static void testCorruptLeafEntries(void)
{
  populateBlockMap(0, BLOCK_COUNT, configureCorruptBlocks);
  performSuccessfulSuspendAndResume(true);
  rebuildReadOnlyVDO();
  verifyBlockMapping(0);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "test reference count rebuild on corrupt leaves", testCorruptLeafEntries },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo vdoSuite = {
  .name = "Reference count rebuild tests (ReferenceCountRebuild_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = teardown,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
