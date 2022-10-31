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
#include "constants.h"
#include "int-map.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "packed-reference-block.h"
#include "recovery-journal-entry.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"

#include "blockMapUtils.h"

#include "asyncLayer.h"
#include "dataBlocks.h"
#include "ioRequest.h"
#include "latchUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  // Test requires 4 pages worth of block map
  BLOCK_MAP_PAGES = 4,
};

typedef struct {
  struct block_map_slot     slot;
  physical_block_number_t   pbn;
} SlotAndPBN;

static struct block_map_slot slot;
static block_count_t         dataBlocks;

static struct slab_depot *depot  = NULL;
static block_count_t      offset = 0;

/**
 * Check whether an AsyncVIO is doing a block map read.
 *
 * Implements WaitCondition.
 **/
static bool isBlockMapRead(void *context)
{
  return (vioTypeIs(context, VIO_TYPE_BLOCK_MAP) && isMetadataRead(context));
}

/**
 * Test-specific initialization.
 **/
static void initializeTest(void)
{
  const TestParameters parameters = {
    // Need at leat two block map pages worth of mappable blocks.
    .mappableBlocks    = VDO_BLOCK_MAP_ENTRIES_PER_PAGE * 2,
    .logicalBlocks     = BLOCK_MAP_PAGES * VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
    .journalBlocks     = 16,
    .slabJournalBlocks = 8,
    // Test relies on this cache size to flush out pages correctly.
    .cacheSize         = 2,
    .dataFormatter     = fillWithOffsetPlusOne,
  };
  initializeVDOTest(&parameters);
  block_count_t logicalBlocks = getTestConfig().config.logical_blocks;
  initializeLatchUtils(DIV_ROUND_UP(logicalBlocks,
                                    VDO_BLOCK_MAP_ENTRIES_PER_PAGE),
                       isBlockMapRead, NULL, NULL);

  // Fill the VDO but save a block for one extra write, and distribute the
  // writes to all 4 block map pages
  dataBlocks = populateBlockMapTree();

  block_count_t toWrite   = dataBlocks - 1;
  block_count_t share     = toWrite / BLOCK_MAP_PAGES;
  block_count_t remainder = toWrite - (share * BLOCK_MAP_PAGES);
  offset                  = 0;
  for (page_count_t i = 0; i < BLOCK_MAP_PAGES; i++) {
    block_count_t size = share + ((i < remainder) ? 1 : 0);
    writeData(VDO_BLOCK_MAP_ENTRIES_PER_PAGE * i, offset, size, VDO_SUCCESS);
    offset += size;
  }
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);

  // Restart the VDO so the journals are effectively empty.
  restartVDO(false);
  depot = vdo->depot;
}

/**
 * Test-specific tear down.
 **/
static void tearDownTest(void)
{
  tearDownLatchUtils();
  tearDownVDOTest();
}

/**
 * Simulate a VDO crash and restart it as dirty using a specific snapshot.
 */
static void rebuildVDOWithSnapshot(PhysicalLayer *snapshot)
{
  stopVDO();
  // Replace the ram layer content with snapshot content.
  copyRAMLayer(getSynchronousLayer(), snapshot);
  // Restart the VDO using a default page cache size.
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  depot = vdo->depot;
}

/**
 * Get the reference count value of a PBN from the slab depot.
 *
 * @param pbn   the physical block number to get reference count for
 */
static vdo_refcount_t getReferenceCount(physical_block_number_t pbn)
{
  struct vdo_slab *slab = vdo_get_slab(depot, pbn);
  slab_block_number slabBlockNumber;
  struct vdo_slab *refcount_slab = slab->reference_counts->slab;
  VDO_ASSERT_SUCCESS(vdo_slab_block_number_from_pbn(refcount_slab,
                                                    pbn, &slabBlockNumber));
  return slab->reference_counts->counters[slabBlockNumber];
}

/**
 * Get the recovery journal entry from the end of journal.
 *
 * @param index   the index of the entry from the end of the journal
 */
static struct packed_recovery_journal_entry *
getEntryBeforeAppendPoint(journal_entry_count_t index)
{
  struct packed_journal_sector *sector
    = vdo->recovery_journal->active_block->sector;
  CU_ASSERT_TRUE(sector->entry_count >= (index + 1));
  return &sector->entries[sector->entry_count - index - 1];
}

/**
 * A hook to record the block map slot.
 *
 * Implements CompletionHook
 **/
static bool recordSlot(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion,
                           VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ)) {
    slot = as_data_vio(completion)->tree_lock.tree_slots[0].block_map_slot,
    removeCompletionEnqueueHook(recordSlot);
  }

  return true;
}

/**
 * Get the block map slot and PBN for the mapping of a given LBN.
 *
 * @param lbn  the logical block to look up
 *
 * @return The result of the lookup
 */
static SlotAndPBN getSlotAndPBN(logical_block_number_t lbn)
{
  SlotAndPBN slotAndPBN;
  addCompletionEnqueueHook(recordSlot);
  slotAndPBN.pbn  = lookupLBN(lbn).pbn;
  slotAndPBN.slot = slot;
  return slotAndPBN;
}

/**********************************************************************/
static void
assertRecoveryJournalEntry(const struct packed_recovery_journal_entry *packed,
                           bool isIncrement,
                           SlotAndPBN mapping)
{
  struct recovery_journal_entry entry
    = vdo_unpack_recovery_journal_entry(packed);
  CU_ASSERT_EQUAL(isIncrement,
                  vdo_is_journal_increment_operation(entry.operation));
  CU_ASSERT_EQUAL(entry.slot.pbn, mapping.slot.pbn);
  CU_ASSERT_EQUAL(entry.slot.slot, mapping.slot.slot);
}

/**
 * Check that decrefs are correctly synthesized.
 *
 * @param lbn  The LBN for which a decref is to be synthesized
 */
static void testSynthesizeDecRef(logical_block_number_t lbn)
{
  SlotAndPBN mapping = getSlotAndPBN(lbn);

  // Issue zero-block writes to two different block map pages to force out the
  // currently cached block map pages from the page cache.
  logical_block_number_t trimLBN = getTestConfig().config.logical_blocks - 1;
  zeroData(trimLBN, 1, VDO_SUCCESS);
  zeroData(trimLBN - VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(),
                  ((mapping.pbn == VDO_ZERO_BLOCK) ? 2 : 1));

  // Launch an overwrite and stop its block map read, which is before the
  // decRefs are added to the recovery journal.
  setLatch(mapping.slot.pbn);
  IORequest *overwrite = launchIndexedWrite(lbn, 1, ++offset);
  waitForLatchedVIO(mapping.slot.pbn);

  // An incRef without a paired decRef is added to the recovery journal.
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(0), true, mapping);

  // Take a snapshot of the current VDO on-disk content.
  PhysicalLayer *missingDecRefs = cloneRAMLayer(getSynchronousLayer());

  releaseLatchedVIO(mapping.slot.pbn);
  clearCompletionEnqueueHooks();

  awaitAndFreeSuccessfulRequest(UDS_FORGET(overwrite));

  SlotAndPBN newMapping = getSlotAndPBN(lbn);

  // The overwrite caused a decRef and an incRef.
  if (mapping.pbn != VDO_ZERO_BLOCK) {
    CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  }
  CU_ASSERT_EQUAL(getReferenceCount(newMapping.pbn), 1);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);

  rebuildVDOWithSnapshot(missingDecRefs);
  missingDecRefs->destroy(&missingDecRefs);

  if (mapping.pbn != VDO_ZERO_BLOCK) {
    CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  }
  CU_ASSERT_EQUAL(getReferenceCount(newMapping.pbn), 1);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);
  struct vdo_statistics stats;
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.logical_blocks_used, dataBlocks + 1);

  // ESC-573: Make sure that if we immediately crash and restart, we don't
  // synthesize the same missing decrefs again (wrongly).
  crashVDO();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  depot = vdo->depot;
  if (mapping.pbn != VDO_ZERO_BLOCK) {
    CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  }
  CU_ASSERT_EQUAL(getReferenceCount(newMapping.pbn), 1);
  CU_ASSERT_EQUAL(getPhysicalBlocksFree(), 1);
  vdo_fetch_statistics(vdo, &stats);
  CU_ASSERT_EQUAL(stats.logical_blocks_used, dataBlocks + 1);
}

/**
 * Missing decRefs can be synthesized from the contents of the recovery
 * journal.
 */
static void testSynthesizeDecRefFromJournal(void)
{
  logical_block_number_t lbn     = 1;
  SlotAndPBN             mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 1);

  // Overwrite lbn once. Two recovery journal entries should be added.
  writeData(lbn, ++offset, 1, VDO_SUCCESS);

  // An incRef is followed by a decRef in the recovery journal.
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(1), true, mapping);

  mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 1);
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(0), false, mapping);

  testSynthesizeDecRef(lbn);
}

/**
 * Missing decRefs can be synthesized from the contents of the block map.
 */
static void testSynthesizeDecRefFromBlockMap(void)
{
  logical_block_number_t lbn     = 1;
  SlotAndPBN             mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 1);

  testSynthesizeDecRef(lbn);
}

/**
 * Missing decRefs might be (unmapped, VDO_ZERO_BLOCK) and should (not) update
 * logicalBlocksUsed correctly.
 */
static void testSynthesizeDecRefOfUnmapped(void)
{
  logical_block_number_t lbn     = 1;
  SlotAndPBN             mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 1);

  // Clear lbn. Two recovery journal entries should be added.
  discardData(lbn, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(mapping.pbn, VDO_ZERO_BLOCK);

  // An incRef is followed by a decRef in the recovery journal.
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(1), true, mapping);
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(0), false, mapping);

  testSynthesizeDecRef(lbn);
}

/**
 * Missing decRefs might be (mapped, VDO_ZERO_BLOCK) and should update
 * logicalBlocksUsed correctly.
 */
static void testSynthesizeDecRefOfZeroes(void)
{
  logical_block_number_t lbn     = 1;
  SlotAndPBN             mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 1);

  // Clear lbn with zeroes. Two recovery journal entries should be added.
  zeroData(lbn, 1, VDO_SUCCESS);
  CU_ASSERT_EQUAL(getReferenceCount(mapping.pbn), 0);
  mapping = getSlotAndPBN(lbn);
  CU_ASSERT_EQUAL(mapping.pbn, VDO_ZERO_BLOCK);

  // An incRef is followed by a decRef in the recovery journal.
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(1), true, mapping);
  assertRecoveryJournalEntry(getEntryBeforeAppendPoint(0), false, mapping);

  testSynthesizeDecRef(lbn);
}

/**********************************************************************/
static CU_TestInfo vdoTests[] = {
  { "Synthesize decRef (journal)",   testSynthesizeDecRefFromJournal  },
  { "Synthesize decRef (block map)", testSynthesizeDecRefFromBlockMap },
  { "Synthesize decRef (unmapped)",  testSynthesizeDecRefOfUnmapped   },
  { "Synthesize decRef (zeroes)",    testSynthesizeDecRefOfZeroes     },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name  = "Synthesize decRef (SynthesizeDecRef_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeTest,
  .cleaner                  = tearDownTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
