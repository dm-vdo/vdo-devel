/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "admin-state.h"
#include "block-allocator.h"
#include "block-map.h"
#include "recovery-journal.h"
#include "ref-counts.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"
#include "vdo-component-states.h"
#include "vdo-recovery.h"

#include "asyncLayer.h"
#include "blockAllocatorUtils.h"
#include "blockMapUtils.h"
#include "callbackWrappingUtils.h"
#include "completionUtils.h"
#include "ioRequest.h"
#include "journalWritingUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  JOURNAL_BLOCKS = 8,
  BLOCK_COUNT    = 8192,
  INJECTED_ERROR = -1,
};

static struct recovery_journal *journal = NULL;
static struct vdo_completion    subTaskCompletion;
static struct waiter            testWaiter;
static struct pooled_vio       *pooled;
static struct slab_journal     *slabJournal;
static bool                     readsComplete;
static bool                     recoveryBlocked;

/** A full block of valid sectors */
const SectorPattern normalSectors[VDO_SECTORS_PER_BLOCK] = {
  { NO_TEAR, EMPTY_SECTOR, GOOD_COUNT, APPLY_NONE }, // Sector 0 has no entries
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, FULL_SECTOR,  GOOD_COUNT, APPLY_ALL  },
  { NO_TEAR, LAST_SECTOR,  GOOD_COUNT, APPLY_ALL  },
};

/**
 * A wrapped journal with head of 16 and tail of 21, used for the slab journal
 * waiting test. No entries will be applied to the block map by construction.
 **/
static BlockPattern slabJournalPattern[JOURNAL_BLOCKS] = {
  {  16,  16, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  16,  17, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  16,  18, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  16,  19, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  16,  20, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  16,  21, GOOD_COUNT, USE_NONCE, FULL_BLOCK, false, normalSectors },
  {  14,  14, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors },
  {  14,  15, GOOD_COUNT, BAD_NONCE, FULL_BLOCK, false, normalSectors },
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
    .slabJournalBlocks   = 8,
    .physicalThreadCount = 1,
  };
  initializeVDOTest(&parameters);

  // Populate the entire block map tree, add slabs, then save and restart
  // the VDO.
  populateBlockMapTree();
  addSlabs(DIV_ROUND_UP(BLOCK_COUNT, vdo->depot->slab_config.data_blocks));
  restartVDO(false);

  journal = vdo->recovery_journal;

  vdo_initialize_completion(&subTaskCompletion, vdo, VDO_SUB_TASK_COMPLETION);
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
static void recoverJournalAction(struct vdo_completion *completion)
{
  vdo_prepare_completion(&subTaskCompletion,
                         vdo_finish_completion_parent_callback,
                         vdo_finish_completion_parent_callback,
                         completion->callback_thread_id,
                         completion);
  vdo->load_state = VDO_DIRTY;
  vdo_repair(&subTaskCompletion);
}

/**********************************************************************/
static void testRebuildSynthesizedDecrefs(void)
{
  initializeJournalWritingUtils(JOURNAL_BLOCKS,
                                getTestConfig().config.logical_blocks,
                                vdo->depot->slab_count - 1);

  /*
   * This tests a very specific scenario from VDO-2310, with only 6 recovery
   * journal entries as follows:
   *
   *   LBN    +/-     PBN
   *     0     +      1
   *  1000     +      2
   *  2000     +      3
   *  2000     -      0
   *  2000     +      4
   *  3000     +      5
   *
   * Hence this test writes a single recovery journal block manually.
   */
  physical_block_number_t journalStart;
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(journal->partition, 0,
                                          &journalStart));

  char           block[VDO_BLOCK_SIZE];
  VDO_ASSERT_SUCCESS(layer->reader(layer, journalStart, 1, block));

  physical_block_number_t   firstDataBlock = vdo->depot->slabs[1]->start;
  struct packed_journal_header *packedHeader
    = (struct packed_journal_header *) block;
  struct packed_journal_sector *sector
    = vdo_get_journal_block_sector(packedHeader, 1);

  struct recovery_block_header header;
  vdo_unpack_recovery_block_header(packedHeader, &header);
  header.block_map_head    = 1;
  header.slab_journal_head = 1;
  header.sequence_number   = 1;
  header.metadata_type     = VDO_METADATA_RECOVERY_JOURNAL;
  header.nonce             = journal->nonce;
  header.entry_count       = 6;
  header.check_byte        = vdo_compute_recovery_journal_check_byte(journal, 1);
  vdo_pack_recovery_block_header(&header, packedHeader);

  sector->check_byte  = header.check_byte;
  sector->entry_count = 6;

  struct packed_recovery_journal_entry *entry = &sector->entries[0];
  makeJournalEntry(entry++,    0,  true,  firstDataBlock + 1, CORRUPT_NOTHING);
  makeJournalEntry(entry++, 1000,  true,  firstDataBlock + 2, CORRUPT_NOTHING);
  makeJournalEntry(entry++, 2000,  true,  firstDataBlock + 3, CORRUPT_NOTHING);
  makeJournalEntry(entry++, 2000, false,                   0, CORRUPT_NOTHING);
  makeJournalEntry(entry++, 2000,  true,  firstDataBlock + 4, CORRUPT_NOTHING);
  makeJournalEntry(entry++, 3000,  true,  firstDataBlock + 5, CORRUPT_NOTHING);

  VDO_ASSERT_SUCCESS(layer->writer(layer, journalStart, 1, block));

  journal->tail = 2;

  // Do a rebuild.
  reset_priority_table(vdo->depot->allocators[0]->prioritized_slabs);
  for (slab_count_t i = 0; i < vdo->depot->slab_count; i++) {
    vdo_free_ref_counts(UDS_FORGET(vdo->depot->slabs[i]->reference_counts));
  }
  performSuccessfulAction(recoverJournalAction);
}

/**
 * This callback implements waiter_callback and is used in
 * signalWhenJournalReadCallbackDone().
 **/
static void acquiredVIO(struct waiter *waiter __attribute__((unused)),
                        void          *vioContext)
{
  CU_ASSERT_PTR_NULL(pooled);
  pooled = (struct pooled_vio *) vioContext;
  broadcast();
}

/**
 * Signals when the one and only slab journal tail block read is done.
 *
 * <p>Implements VDOAction.
 **/
static void signalWhenJournalReadCallbackDone(struct vdo_completion *completion)
{
  struct block_allocator *allocator = slabJournal->slab->allocator;
  testWaiter.callback = acquiredVIO;
  acquire_vio_from_pool(allocator->vio_pool, &testWaiter);
  signalState(&readsComplete);
  runSavedCallback(completion);
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfJournalRead(struct vdo_completion *completion)
{
  if (onBIOThread()
      && isMetadataRead(completion)
      && vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL)) {
    wrapCompletionCallback(completion, signalWhenJournalReadCallbackDone);
    clearCompletionEnqueueHooks();
  }

  return true;
}

/**
 * Implements BlockCondition.
 **/
static bool
isSlabJournalWrite(struct vdo_completion *completion,
                   void                  *context __attribute__((unused)))
{
  return (vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL)
          && isMetadataWrite(completion));
}

/**
 * An action to release the reserved VIO pool entry.
 **/
static void releaseVIOPoolEntryAction(struct vdo_completion *completion)
{
  return_vio_to_pool(slabJournal->slab->allocator->vio_pool, pooled);
  pooled = NULL;
  vdo_complete_completion(completion);
}

/**
 * Verify that the slab journal for slab 1 contains all the entries we expect.
 **/
static void verifySlabJournalEntries(void)
{
  struct slab_depot       *depot          = vdo->depot;
  struct vdo_slab         *slab           = depot->slabs[1];
  physical_block_number_t  slabJournalPBN = slab->journal_origin + 1;
  sequence_number_t        sequenceNumber = 1;

  journal_entry_count_t totalIncrements   = 6 * journal->entries_per_block;
  journal_entry_count_t remainingEntries  = totalIncrements * 2;

  // The pattern written uses 7 * 46 LBNs to write each block, although only
  // 311 entries are actually useful.
  logical_block_number_t lbnsPerRecoveryJournalBlock
    = (RECOVERY_JOURNAL_ENTRIES_PER_SECTOR * (VDO_SECTORS_PER_BLOCK - 1));

  logical_block_number_t nextLBN           = 0;
  bool                   expectedIncrement = true;

  char buffer[VDO_BLOCK_SIZE];
  while (remainingEntries > 0) {
    VDO_ASSERT_SUCCESS(layer->reader(layer, slabJournalPBN++, 1, buffer));
    struct packed_slab_journal_block *block
      = (struct packed_slab_journal_block *) buffer;
    struct slab_journal_block_header header;
    vdo_unpack_slab_journal_block_header(&block->header, &header);
    CU_ASSERT_EQUAL(header.sequence_number, sequenceNumber++);
    CU_ASSERT_EQUAL(header.entry_count,
                    min(remainingEntries,
                        slab->journal->entries_per_block));
    for (journal_entry_count_t i = 0; i < header.entry_count; i++) {
      struct slab_journal_entry entry
        = vdo_decode_slab_journal_entry(block, i);
      slab_block_number expectedSBN
        = (computePBNFromLBN(nextLBN, (expectedIncrement ? 1 : 0))
           - slab->start);

      CU_ASSERT_EQUAL((entry.operation == VDO_JOURNAL_DATA_INCREMENT),
                      expectedIncrement);
      CU_ASSERT_EQUAL(entry.sbn, expectedSBN);

      expectedIncrement ? nextLBN++ : nextLBN--;
      remainingEntries--;
      if (remainingEntries == totalIncrements) {
        nextLBN--;
        expectedIncrement = false;
      }

      // Skip the holes in the LBN space due to the writing process filling
      // every entry in every sector.
      if ((nextLBN % lbnsPerRecoveryJournalBlock)
          >= journal->entries_per_block) {
        if (expectedIncrement) {
          nextLBN += (lbnsPerRecoveryJournalBlock - journal->entries_per_block);
        } else {
          nextLBN -= (lbnsPerRecoveryJournalBlock - journal->entries_per_block);
        }
      }
    }
  }
}

/**********************************************************************/
static void checkForRecoveryBlocked(struct vdo_completion *completion)
{
  if (vdo_get_admin_state_code(&slabJournal->slab->state)
      == VDO_ADMIN_STATE_WAITING_FOR_RECOVERY) {
    signalState(&recoveryBlocked);
  }

  vdo_complete_completion(completion);
}

/**
 * Assert that the slab journal block's recovery journal point matches the
 * given parameters.
 *
 * @param blockNumber The expected slab journal point sequence number
 * @param entryCount  the expected slab journal point entry count
 **/
static void assertSlabJournalPoint(sequence_number_t     blockNumber,
                                   journal_entry_count_t entryCount)
{
  struct journal_point recoveryPoint = slabJournal->tail_header.recovery_point;
  CU_ASSERT_EQUAL(blockNumber, recoveryPoint.sequence_number);
  CU_ASSERT_EQUAL(entryCount, recoveryPoint.entry_count);
}

/**
 * Test rebuild's behavior when a slab journal runs out of space to add
 * new entries.
 **/
static void testWaitForSlabJournalSpace(void)
{
  // For ease of testing, we use only one slab / slab journal.
  initializeJournalWritingUtils(JOURNAL_BLOCKS,
                                getTestConfig().config.logical_blocks, 1);

  // Perform the standard setup for the recovery action.
  putBlocksInMap(0, BLOCK_COUNT);
  verifyBlockMapping(0);
  writeJournalBlocks(CORRUPT_NOTHING, false, slabJournalPattern);

  struct block_allocator *allocator = vdo->depot->allocators[0];
  reset_priority_table(allocator->prioritized_slabs);

  for (slab_count_t i = 0; i < vdo->depot->slab_count; i++) {
    vdo_free_ref_counts(UDS_FORGET(vdo->depot->slabs[i]->reference_counts));
  }

  // Use a single-VIO pool so it's easy to keep the slab journal from having
  // a VIO to write with.

  reserveVIOsFromPool(allocator, BLOCK_ALLOCATOR_VIO_POOL_SIZE - 1);

  /*
   * Set up a hook to notice when each slab journal read finishes.  When the
   * callback for the last read is done, we'll snag the vio pool entry so the
   * slab journal can't write any blocks. The callback wrapping will signal
   * when this has occurred. The recovery will then replay until
   * the slab journal has filled its first block and needs to issue a write
   * before we can replay any more entries.
   */
  readsComplete        = false;
  recoveryBlocked      = false;
  slabJournal          = vdo->depot->slabs[1]->journal;
  setCompletionEnqueueHook(wrapIfJournalRead);

  // Launch the recovery.
  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  launchAction(recoverJournalAction, &completion);

  /*
   * Wait for the first blockful of entries to be played and the completion
   * to begin waiting. Verify the slab journal is accurate up to the expected
   * point in the recovery journal, confirming that we replayed only one block
   * full of entries into the slab journal before blocking.
   */
  waitForStateAndClear(&readsComplete);
  while (!checkState(&recoveryBlocked)) {
    performSuccessfulActionOnThread(checkForRecoveryBlocked,
                                    allocator->thread_id);
  }
  clearState(&recoveryBlocked);
  assertSlabJournalPoint(20, 108);

  /*
   * Set up a hook to block the first slab journal write.
   */
  setBlockBIO(isSlabJournalWrite, false);

  // Let go of the VIO pool entry; it will be issued and then blocked.
  // Recovery will replay another blockful, then be out of space again.
  performSuccessfulActionOnThread(releaseVIOPoolEntryAction,
                                  allocator->thread_id);
  while (!checkState(&recoveryBlocked)) {
    performSuccessfulActionOnThread(checkForRecoveryBlocked,
                                    allocator->thread_id);
  }
  clearState(&recoveryBlocked);
  // Verify exactly one blockful was replayed.
  assertSlabJournalPoint(21, 1150);

  // Release the first slab journal write. The block will be reused for
  // the second block, and replay will finish.
  struct vio *blockedVIO = getBlockedVIO();
  reallyEnqueueBIO(blockedVIO->bio);

  // Release the second slab journal write. The block will be reused for
  // the third block.
  blockedVIO = getBlockedVIO();
  reallyEnqueueBIO(blockedVIO->bio);

  /*
   * Set an error on the last slab journal write to terminate recovery, and
   * release all the remaining pooled VIOs since recovery concludes by
   * draining the slab depot which expects the VIO pool to not be busy.
   */
  blockedVIO = getBlockedVIO();
  vdo_set_completion_result(vio_as_completion(blockedVIO), INJECTED_ERROR);
  returnVIOsToPool();
  reallyEnqueueBIO(blockedVIO->bio);

  // Make sure the recovery did exactly the expected amount of work.
  awaitCompletion(&completion);
  assertSlabJournalPoint(21, 7 * journal->entries_per_block - 1);

  // Make sure the slab journal got the expected entries.
  verifySlabJournalEntries();
  // Make sure nothing happened to the block map.
  verifyBlockMapping(0);

  setStartStopExpectation(VDO_READ_ONLY);

  returnVIOsToPool();
}

/**********************************************************************/
static CU_TestInfo journalRebuildTests[] = {
  { "rebuild with synthesized decrefs",     testRebuildSynthesizedDecrefs },
  { "rebuild with waiting during replay",   testWaitForSlabJournalSpace   },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "Recover into slab journals (SlabJournalRecovery_t1)",
  .initializer = initializeRebuildTest,
  .cleaner     = tearDownRebuildTest,
  .tests       = journalRebuildTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
