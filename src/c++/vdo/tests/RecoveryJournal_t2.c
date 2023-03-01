/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "block-map.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab-depot.h"
#include "vdo.h"
#include "vdo-recovery.h"
#include "vio.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "completionUtils.h"
#include "ioRequest.h"
#include "journalWritingUtils.h"
#include "ramLayer.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  JOURNAL_BLOCKS = 8,
  BLOCK_COUNT    = 8192,
};

static struct recovery_journal     *journal  = NULL;

/** A full block of valid sectors */
const SectorPattern normalSectors[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR, EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // Sector 0 has no entries
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
};

/** A full block with one sector containing an incorrect check byte */
const SectorPattern badCheckSector[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR,  EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // No entries in sector 0
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR,  FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
};

/** A full block with one sector containing an incorrect recovery count */
const SectorPattern badCountSector[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR, EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // Sector 0 has no entries
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  BAD_COUNT,  APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
};

/**
 * A full block with one sector short, as if written once and then torn
 * on the second write.
 **/
const SectorPattern shortSector[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR, EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // Sector 0 has no entries
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, SHORT_SECTOR, GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
};

/** A block where all sectors are full but the header's entry count is short */
const SectorPattern shortBlockSectors[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR, EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // Sector 0 has no entries
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_PART },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
};

/**
 * A full block with every sector containing an incorrect check byte, as if
 * only the first sector with the header were committed.
 **/
const SectorPattern noSectors[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR,  EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // No entries in sector 0
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
  { TEAR_OLD, FULL_SECTOR,  GOOD_COUNT, APPLY_NONE },
};

/**
 * A wrapped journal with a reap head at block 6 and the tail at a partial
 * block1. The reap head is 14 and the highest sequence number is 17.
 **/
static BlockPattern shortBlockJournalTailPattern[JOURNAL_BLOCKS] = {
  { 14, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  { 11, 17, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, true,  shortSector   },
  {  0, 50, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  5, 11, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  { 11, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
};

/**
 * A non-wrapped journal with a hole at the reap head. The hole is a
 * block with a bad nonce. The reap head is 2 and the highest sequence
 * number is 5.
 **/
static BlockPattern holeAtReapHeadPattern[JOURNAL_BLOCKS] = {
  {  1, 20, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  1, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  2, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  3, BAD_COUNT,  USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  5, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, false, shortSector   },
  {  0,  0, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  0,  0, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * A non-wrapped journal with a hole at block4 in the middle of the
 * journal. The reap head is 2 and the highest sequence number is 6.
 **/
static BlockPattern holeMidJournalPattern[JOURNAL_BLOCKS] = {
  {  0, 16, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  1, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  2, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  {  1,  3, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  {  1,  4, BAD_COUNT,  USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  2,  5, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  6, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, false, shortSector   },
  {  0,  0, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * A wrapped journal with a hole at block0 just before the highest tail
 * value. The reap head is 14 and the highest sequence number is 17.
 **/
static BlockPattern holeBeforeTailPattern[JOURNAL_BLOCKS] = {
  {  1,  8, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  { 12, 17, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, false, shortSector   },
  {  1, 18, BAD_COUNT,  USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  5, 11, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  { 14, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
};

/**
 * A non-wrapped journal with two holes, the first, at block 1, is a partially
 * written reap head and the second one, at block 4, is a block with a bad
 * recovery count. The reap head is 1 and the highest sequence number is 5.
 **/
static BlockPattern twoHolesJournalPattern[JOURNAL_BLOCKS] = {
  {  0,  0, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  1, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, true,  shortSector   },
  {  1,  2, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  3, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 12, BAD_COUNT,  USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  5, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  0,  0, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  0,  0, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * A wrapped journal with many holes for read-only rebuild.
 * The reap head is 12 and the highest sequence number is 17.
 *
 * Block0 has a later head (reap point) than last valid block.
 * Block1 is the last applicable journal block and is partially full.
 * Block2 represents a block left over from a previous VDO incarnation.
 * Block3 is outside the active journal.
 * Block4 is the first block of the active journal.
 * Block5 was not written since the last time on-disk journal wrapped.
 * Block6 is valid but not completely full.
 * Block7 was not written since the last format.
 **/
static BlockPattern readOnlyRebuildPattern[JOURNAL_BLOCKS] = {
  { 12, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  { 11, 17, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, true,  shortSector   },
  { 20, 26, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  5, 11, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  7, 12, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors },
  {  1,  5, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 14, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, true,  shortSector   },
  { 11, 15, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * A completely invalid journal for read-only rebuild. Every block has
 * a bad nonce, and otherwise this is identical to readOnlyRebuildPattern.
 **/
static BlockPattern emptyReadOnlyRebuildPattern[JOURNAL_BLOCKS] = {
  { 12, 16, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  { 11, 17, GOOD_COUNT, BAD_NONCE, SHORT_BLOCK, false, shortSector   },
  { 20, 26, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  5, 11, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  7, 12, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  5, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 14, GOOD_COUNT, BAD_NONCE, SHORT_BLOCK, false, shortSector   },
  { 11, 15, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * An entry-free journal for read-only rebuild. All but one block has
 * a bad nonce, and the remaining block has no valid sectors.
 **/
static BlockPattern noEntryReadOnlyRebuildPattern[JOURNAL_BLOCKS] = {
  { 12, 16, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  { 11, 17, GOOD_COUNT, BAD_NONCE, SHORT_BLOCK, false, shortSector   },
  { 20, 26, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  5, 11, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, noSectors     },
  {  7, 12, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  1,  5, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
  {  9, 14, GOOD_COUNT, BAD_NONCE, SHORT_BLOCK, false, shortSector   },
  { 11, 15, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors },
};

/**
 * A journal with a torn write resulting in a sector in the middle of
 * a journal block with a bad check byte. The reap head is 14 and the
 * highest sequence number is 18.
 **/
static BlockPattern badCheckSectorPattern[JOURNAL_BLOCKS] = {
  { 13, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
  { 14, 17, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  badCheckSector },
  { 11, 18, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors  },
  { 46, 51, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors  },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors  },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors  },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
  { 11, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
};

/**
 * A journal with a torn write resulting in a sector in the middle of
 * a journal block with a bad recovery count. The reap head is 14 and
 * the highest sequence number is 18.
 **/
static BlockPattern badCountSectorPattern[JOURNAL_BLOCKS] = {
  { 13, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
  { 14, 17, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  badCountSector },
  { 11, 18, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors  },
  { 46, 55, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors  },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors  },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors  },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
  { 11, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors  },
};

/**
 * A journal with a torn write resulting in a short sector in the middle of a
 * journal block. The reap head is 14 and the highest sequence number is 18.
 **/
static BlockPattern partialSectorPattern[JOURNAL_BLOCKS] = {
  { 13, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors },
  { 14, 17, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  shortSector   },
  { 11, 18, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  { 46, 51, BAD_COUNT,  USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors },
  { 11, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK, true,  normalSectors },
};

/**
 * A journal with a torn write that failed to update the header sector of a
 * journal block. The reap head is 14 and the highest sequence number is 18.
 **/
static BlockPattern partialHeaderPattern[JOURNAL_BLOCKS] = {
  { 13, 16, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors     },
  { 14, 17, GOOD_COUNT, USE_NONCE, SHORT_BLOCK, true,  shortBlockSectors },
  { 11, 18, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors     },
  { 46, 51, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors     },
  {  2,  4, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  false, normalSectors     },
  {  1, 13, GOOD_COUNT, BAD_NONCE, FULL_BLOCK,  false, normalSectors     },
  {  9, 14, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors     },
  { 11, 15, GOOD_COUNT, USE_NONCE, FULL_BLOCK,  true,  normalSectors     },
};

/**
 * Initialize the index, vdo, and test data.
 **/
static void initializeRebuildTest(void)
{
  TestParameters parameters = {
    .logicalBlocks       = BLOCK_COUNT,
    .slabCount           = 1,
    .slabSize            = 1024,
    .journalBlocks       = JOURNAL_BLOCKS,
    .physicalThreadCount = 1,
  };
  initializeVDOTest(&parameters);

  // Populate the entire block map tree, add slabs, then save and restart
  // the vdo.
  populateBlockMapTree();
  addSlabs(DIV_ROUND_UP(BLOCK_COUNT, vdo->depot->slab_config.data_blocks));
  restartVDO(false);

  journal = vdo->recovery_journal;
  initializeJournalWritingUtils(JOURNAL_BLOCKS,
                                getTestConfig().config.logical_blocks,
                                vdo->depot->slab_count - 1);
}

/**
 * Destroy the test data, vdo, and index session.
 **/
static void tearDownRebuildTest(void)
{
  tearDownJournalWritingUtils();
  tearDownVDOTest();
}

/**********************************************************************/
static void rebuildJournalAction(struct vdo_completion *completion)
{
  vdo->load_state = VDO_FORCE_REBUILD;
  vdo_repair(completion);
}

/**********************************************************************/
static void recoverJournalAction(struct vdo_completion *completion)
{
  vdo->load_state = VDO_DIRTY;
  vdo_repair(completion);
}

/**********************************************************************/
static void checkReplayingAction(struct vdo_completion *completion)
{
  if (vdo_get_state(vdo) == VDO_REPLAYING) {
    setStartStopExpectation(UDS_BAD_STATE);
  }

  vdo_complete_completion(completion);
}

/**
 * Create a block map with a known pattern, then set up journal entries.
 * Show that the valid journal mappings are applied to the block map while
 * all others are ignored.
 *
 * @param  corruption      the type of corruption to apply to journal entries
 * @param  readOnly        whether this is a rebuild out of read-only mode
 * @param  journalPattern  the journal block pattern
 **/
static void attemptRebuild(CorruptionType  corruption,
                           bool            readOnly,
                           BlockPattern   *journalPattern)
{
  putBlocksInMap(0, BLOCK_COUNT);
  verifyBlockMapping(0);
  writeJournalBlocks(corruption, readOnly, journalPattern);

  // Attempt to rebuild the block map from the journal.
  if (readOnly) {
    // Make VDO to do a full rebuild.
    vdo->load_state = VDO_FORCE_REBUILD;
  }
  reset_priority_table(vdo->depot->allocators[0].prioritized_slabs);

  if (readOnly || (corruption == CORRUPT_NOTHING)) {
    // Free all the refcounts, so the expected amount of the slab depot is
    // allocated before rebuild/recovery allocates the rest.
    for (slab_count_t i = 0; i < vdo->depot->slab_count; i++) {
      vdo_free_ref_counts(UDS_FORGET(vdo->depot->slabs[i]->reference_counts));
    }
  }

  if (readOnly) {
    performSuccessfulAction(rebuildJournalAction);
  } else if (corruption == CORRUPT_NOTHING) {
    performSuccessfulAction(recoverJournalAction);
    performSuccessfulAction(checkReplayingAction);
  } else {
    performActionExpectResult(recoverJournalAction, VDO_CORRUPT_JOURNAL);
    setStartStopExpectation(VDO_READ_ONLY);
  }

  verifyBlockMapping(0);

  if (!readOnly && (corruption != CORRUPT_NOTHING)) {
    // Corruption during normal rebuild should throw VDO into read-only mode.
    performSuccessfulAction(vdo_wait_until_not_entering_read_only_mode);
    checkVDOState(VDO_READ_ONLY_MODE);
  }
}

/**********************************************************************/
static void testRebuildShortBlock(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, shortBlockJournalTailPattern);
}

/**********************************************************************/
static void testRebuildHoleAtReapHead(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, holeAtReapHeadPattern);
}

/**********************************************************************/
static void testRebuildHoleMidJournal(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, holeMidJournalPattern);
}

/**********************************************************************/
static void testRebuildHoleBeforeTail(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, holeBeforeTailPattern);
}

/**********************************************************************/
static void testRebuildTwoHoles(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, twoHolesJournalPattern);
}

/**********************************************************************/
static void testReadOnlyRebuild(void)
{
  attemptRebuild(CORRUPT_NOTHING, true, readOnlyRebuildPattern);
}

/**********************************************************************/
static void testNoJournalReadOnlyRebuild(void)
{
  attemptRebuild(CORRUPT_NOTHING, true, emptyReadOnlyRebuildPattern);
}

/**********************************************************************/
static void testNoEntryReadOnlyRebuild(void)
{
  attemptRebuild(CORRUPT_NOTHING, true, noEntryReadOnlyRebuildPattern);
}

/**********************************************************************/
static void testCorruptLBNSlots(void)
{
  attemptRebuild(CORRUPT_LBN_SLOT, false, shortBlockJournalTailPattern);
}

/**********************************************************************/
static void testCorruptLBNSlotsReadOnly(void)
{
  attemptRebuild(CORRUPT_LBN_SLOT, true, readOnlyRebuildPattern);
}

/**********************************************************************/
static void testCorruptLBNPBNs(void)
{
  attemptRebuild(CORRUPT_LBN_PBN, false, shortBlockJournalTailPattern);
}

/**********************************************************************/
static void testCorruptLBNPBNsReadOnly(void)
{
  attemptRebuild(CORRUPT_LBN_PBN, true, readOnlyRebuildPattern);
}

/**********************************************************************/
static void testCorruptPBNs(void)
{
  attemptRebuild(CORRUPT_PBN, false, shortBlockJournalTailPattern);
}

/**********************************************************************/
static void testCorruptPBNsReadOnly(void)
{
  attemptRebuild(CORRUPT_PBN, true, readOnlyRebuildPattern);
}

/**********************************************************************/
static void testBadCheckByteSector(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, badCheckSectorPattern);
}

/**********************************************************************/
static void testBadCountByteSector(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, badCountSectorPattern);
}

/**********************************************************************/
static void testPartialSector(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, partialSectorPattern);
}

/**********************************************************************/
static void testPartialHeader(void)
{
  attemptRebuild(CORRUPT_NOTHING, false, partialHeaderPattern);
}

/**********************************************************************/
static CU_TestInfo journalRebuildTests[] = {
  { "rebuild block map with short block",   testRebuildShortBlock         },
  { "rebuild with a hole at reap head",     testRebuildHoleAtReapHead     },
  { "rebuild with a hole mid-journal",      testRebuildHoleMidJournal     },
  { "rebuild with a hole before the tail",  testRebuildHoleBeforeTail     },
  { "rebuild with journal with two holes",  testRebuildTwoHoles           },
  { "read-only rebuild",                    testReadOnlyRebuild           },
  { "read-only rebuild with no journal",    testNoJournalReadOnlyRebuild  },
  { "read-only rebuild with no entries",    testNoEntryReadOnlyRebuild    },
  { "corrupt logical slots",                testCorruptLBNSlots           },
  { "corrupt logical slots (read-only)",    testCorruptLBNSlotsReadOnly   },
  { "corrupt logical PBNs",                 testCorruptLBNPBNs            },
  { "corrupt logical PBNs (read-only)",     testCorruptLBNPBNsReadOnly    },
  { "corrupt physical blocks",              testCorruptPBNs               },
  { "corrupt physical blocks (read-only)",  testCorruptPBNsReadOnly       },
  { "rebuild with bad sector (check byte)", testBadCheckByteSector        },
  { "rebuild with bad sector (count)",      testBadCountByteSector        },
  { "rebuild with partial sector",          testPartialSector             },
  { "rebuild with partial header",          testPartialHeader             },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo recoveryJournalSuite = {
  .name                     = "Rebuild from journal (RecoveryJournal_t2)",
  .initializerWithArguments = NULL,
  .initializer              = initializeRebuildTest,
  .cleaner                  = tearDownRebuildTest,
  .tests                    = journalRebuildTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &recoveryJournalSuite;
}
