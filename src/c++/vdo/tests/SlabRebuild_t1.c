/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "ref-counts.h"
#include "recovery-journal.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "slab-summary.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "completionUtils.h"
#include "mutexUtils.h"
#include "ramLayer.h"
#include "testParameters.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

// This affects the actual number of reference blocks.
static const block_count_t         SLAB_SIZE           = 4 * 4096;
static const block_count_t         SLAB_JOURNAL_BLOCKS = 8;
// There are only four full reference blocks
static const block_count_t         REFCOUNT_BLOCKS     = 4;
static const journal_entry_count_t SHORT_BLOCK_COUNT   = 32;

static const uint8_t DEFAULT_REFERENCE_COUNT = 100;
static const bool IS_VALID[]
  = {true, true, false, false, true, true, true, true};

static struct slab_depot     *depot              = NULL;
static struct vdo_slab       *slab               = NULL;
static struct slab_journal   *journal            = NULL;
static vdo_refcount_t        *expectedReferences = NULL;

static bool                   latchRead;

static block_count_t          expectedBlocksFree;
static struct slab_config     slabConfig;

// These are the commit points of the reference count blocks.
static struct journal_point blockLimits[5] = {
  {  1,   0 }, // before the start of the journal
  { 14,   0 }, // in the middle of the journal
  { 16, 160 }, // in the middle of a block
  { 16, 161 }, // in the middle of a block
  { 17,  31 }, // at the end of the journal
};

typedef struct {
  struct vdo_completion actionCompletion;
  struct vdo_completion completion;
  struct data_vio       dataVIO;
} DataVIOWrapper;

/**
 * Initialize the index, VDO, and test data.
 **/
static void initializeRebuildTest(void)
{
  TestParameters parameters = {
    .slabSize          = SLAB_SIZE,
    .slabCount         = 1,
    .slabJournalBlocks = SLAB_JOURNAL_BLOCKS,
  };
  initializeVDOTest(&parameters);

  depot      = vdo->depot;
  slabConfig = depot->slab_config;
  slab       = depot->slabs[0];
  journal    = slab->journal;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(slabConfig.data_blocks, uint8_t, __func__,
                                  &expectedReferences));
  latchRead    = true;
}

/**
 * Destroy the test data, VDO, and index session.
 **/
static void teardownRebuildTest(void)
{
  UDS_FREE(expectedReferences);
  tearDownVDOTest();
}

/**********************************************************************/
static void loadRefCounts(struct vdo_completion *completion)
{
  // mark the ref counts for loading
  slab->allocator->summary->entries[slab->slab_number].load_ref_counts = true;
  vdo_reset_reference_counts(slab->reference_counts);
  vdo_start_draining(&slab->state, VDO_ADMIN_STATE_SCRUBBING, completion,
                     NULL);
  vdo_drain_ref_counts(slab->reference_counts);
}

/**
 * Cause the test to fail if the slab rebuild fails. This catches the failure
 * at a point where it is easier to see what actually went wrong.
 *
 * @param completion  The rebuild completion
 **/
static void failOnError(struct vdo_completion *completion)
{
  CU_FAIL("Scrubbing failed with result: %d", completion->result);
}

/**********************************************************************/
static void scrubSlabAction(struct vdo_completion *completion)
{
  // Mark the slab summary to indicate the slab is unrecovered.
  VDO_ASSERT_SUCCESS(initialize_slab_scrubber(slab->allocator));
  slab->allocator->summary->entries[slab->slab_number].is_dirty = true;
  slab->status = VDO_SLAB_REQUIRES_SCRUBBING;
  vdo_register_slab_for_scrubbing(slab, true);
  wrapCompletionCallbackAndErrorHandler(completion, runSavedCallbackAssertNoRequeue, failOnError);
  scrub_slabs(slab->allocator, completion);
}

/**
 * Fill the reference counts with fixed data so that we can determine how
 * many references are applied from the slab journal. The initial state must
 * ensure that reference counts are high enough to apply all decrefs if
 * necessary.
 **/
static void initializeReferenceCounts(void)
{
  // Write the reference count block directly to the layer.
  physical_block_number_t  pbn             = slab->ref_counts_origin;
  block_count_t            blocksRemaining = slabConfig.data_blocks;

  // Leave block 0 half empty and half provisional for block map increments
  for (block_count_t i = 0; blocksRemaining > 0; i++) {
    char buffer[VDO_BLOCK_SIZE];
    memset(buffer, 0, VDO_BLOCK_SIZE);
    struct packed_reference_block *block
      = (struct packed_reference_block *) buffer;
    for (sector_count_t j = 0; j < VDO_SECTORS_PER_BLOCK; j++) {
      block_count_t countsToSet
        = min(blocksRemaining, (block_count_t) COUNTS_PER_SECTOR);
      vdo_pack_journal_point(&blockLimits[i], &block->sectors[j].commit_point);
      vdo_refcount_t count = DEFAULT_REFERENCE_COUNT;
      if (i == 0) {
        count = ((j < VDO_SECTORS_PER_BLOCK / 2)
                 ? EMPTY_REFERENCE_COUNT : PROVISIONAL_REFERENCE_COUNT);
      }
      memset(&block->sectors[j].counts, count, countsToSet);
      blocksRemaining -= countsToSet;
    }
    VDO_ASSERT_SUCCESS(layer->writer(layer, pbn + i, 1, buffer));
  }

  // Load the reference counts so that the in-memory state matches the layer.
  performSuccessfulAction(loadRefCounts);

  memcpy(expectedReferences, slab->reference_counts->counters,
         slabConfig.data_blocks);

  // The load should wipe out the provisional reference counts
  expectedBlocksFree = COUNTS_PER_BLOCK;
}

/**********************************************************************/
static void verifyReferences(void)
{
  CU_ASSERT_EQUAL(expectedBlocksFree, slab->reference_counts->free_blocks);
  for (block_count_t i = 0; i < slabConfig.data_blocks; i++ ) {
    CU_ASSERT_EQUAL(expectedReferences[i], slab->reference_counts->counters[i]);
  }
}

/**********************************************************************/
static journal_entry_count_t
setHeader(struct slab_journal_block_header *header, uint16_t number)
{
  header->metadata_type = VDO_METADATA_SLAB_JOURNAL;
  header->nonce         = depot->allocators[0].nonce;
  switch (number) {
  case 0:
    // This block is completely valid, but has a later head (reap
    // point) than the last valid journal block.
    header->head            = 13;
    header->sequence_number = 16;
    return journal->entries_per_block;

  case 1:
    // This is the last valid journal block written.  It is also
    // not completely full, so only the valid entries will be used.
    header->head            = 12;
    header->sequence_number = 17;
    return SHORT_BLOCK_COUNT;

  case 2:
    // This block is completely valid but is outside the active journal.
    header->head            = 5;
    header->sequence_number = 10;
    return journal->entries_per_block;

  case 3:
    // This block is completely valid but is outside the active journal.
    header->head            = 5;
    header->sequence_number = 11;
    return SHORT_BLOCK_COUNT;

  case 4:
    // This block is completely valid, and is the first block of the
    // active journal.
    header->head            = 7;
    header->sequence_number = 12;
    return journal->entries_per_block;

  case 5:
    // This block is full, valid, and has block map increments.
    header->head                     = 7;
    header->sequence_number          = 13;
    header->has_block_map_increments = true;
    return journal->full_entries_per_block;

  case 6:
    // This block is completely valid, but not completely full.
    header->head            = 9;
    header->sequence_number = 14;
    return SHORT_BLOCK_COUNT;

  case 7:
    // This block is both full and valid.
    header->head            = 11;
    header->sequence_number = 15;
    return journal->entries_per_block;

  default:
    CU_FAIL("Invalid block number: %u", number);
  }
}

/**
 * Select the journal operation and reference block for the next entry.
 *
 * @param header  The journal block header
 *
 * @return The reference updater
 **/
static struct reference_updater
selectOperationAndBlock(const struct slab_journal_block_header *header)
{
  struct reference_updater updater = {
    .operation = VDO_JOURNAL_DATA_REMAPPING,
    .increment = true,
    .zpbn = {
      .pbn = (header->entry_count % (REFCOUNT_BLOCKS - 1)) + 1,
    },
  };
  if ((header->entry_count % (2 * REFCOUNT_BLOCKS)) >= REFCOUNT_BLOCKS) {
    updater.increment = false;
    return updater;
  }

  if (header->has_block_map_increments && ((header->entry_count % 3) == 0)) {
    updater.operation = VDO_JOURNAL_BLOCK_MAP_REMAPPING;
    updater.zpbn.pbn  = 0;
  }

  return updater;
}

static unsigned int get_offset(enum journal_operation operation, bool increment)
{
  if (operation == VDO_JOURNAL_BLOCK_MAP_REMAPPING) {
    return 0;
  }

  if (increment) {
    return 1;
  }

  return 2;
}

/**
 * Create slab journal blocks that represent interesting journal
 * configurations and write then to the layer.
 */
static void writeSlabJournalBlocks(void)
{
  // Write to the layer directly.
  physical_block_number_t pbn = slab->journal_origin;

  // Offsets for each type of journal operation.
  slab_block_number offsets[3];
  offsets[get_offset(VDO_JOURNAL_BLOCK_MAP_REMAPPING, true)] = 0;
  offsets[get_offset(VDO_JOURNAL_DATA_REMAPPING, false)]     = COUNTS_PER_BLOCK / 2;
  offsets[get_offset(VDO_JOURNAL_DATA_REMAPPING, true)]      = 0;

  // Initialize the block entries.
  for (block_count_t i = 0; i < SLAB_JOURNAL_BLOCKS; i++) {
    char buffer[VDO_BLOCK_SIZE];
    memset(buffer, 0, VDO_BLOCK_SIZE);
    struct packed_slab_journal_block *block
      = (struct packed_slab_journal_block *) buffer;

    struct slab_journal_block_header header;
    memset(&header, 0, sizeof(header));
    journal_entry_count_t entryCount = setHeader(&header, i);

    // Set all entries, valid or not, to unique values so we can
    // determine later which entries have been applied.
    journal_entry_count_t entries = (header.has_block_map_increments
                                     ? journal->full_entries_per_block
                                     : journal->entries_per_block);
    while (header.entry_count < entries) {
      struct reference_updater updater = selectOperationAndBlock(&header);
      slab_block_number sbn = (updater.zpbn.pbn * COUNTS_PER_BLOCK);
      sbn += offsets[get_offset(updater.operation, updater.increment)];
      /*
       * For data updates, increment the offset whenever we get to the end.
       * For block map updates, increment every time since any given block
       * map block can only be incremented once.
       */
      if ((updater.zpbn.pbn == 0) || (updater.zpbn.pbn == (REFCOUNT_BLOCKS - 1))) {
        offsets[get_offset(updater.operation, updater.increment)]++;
      }

      struct journal_point currentPoint = {
        .sequence_number = header.sequence_number,
        .entry_count     = header.entry_count,
      };
      if (IS_VALID[i] && (header.entry_count < entryCount)
          && vdo_before_journal_point(&blockLimits[updater.zpbn.pbn], &currentPoint)) {
        if (updater.operation == VDO_JOURNAL_BLOCK_MAP_REMAPPING) {
          CU_ASSERT_EQUAL(expectedReferences[sbn], 0);
          expectedReferences[sbn] = MAXIMUM_REFERENCE_COUNT;
          expectedBlocksFree--;
        } else if (updater.increment) {
          CU_ASSERT_TRUE(expectedReferences[sbn] < MAXIMUM_REFERENCE_COUNT);
          expectedReferences[sbn]++;
        } else {
          CU_ASSERT_TRUE(expectedReferences[sbn] > 0);
          expectedReferences[sbn]--;
        }
      }

      vdo_encode_slab_journal_entry(&header,
                                    &block->payload,
                                    sbn,
                                    updater.operation,
                                    updater.increment);

      // The header hasn't been packed yet, but decoding from the block
      // requires the hasBlockMapIncrements field from the header.
      block->header.has_block_map_increments = header.has_block_map_increments;

      struct slab_journal_entry decoded
        = vdo_decode_slab_journal_entry(block, header.entry_count - 1);
      CU_ASSERT_EQUAL(decoded.sbn, sbn);
      CU_ASSERT_EQUAL(decoded.operation, updater.operation);
      CU_ASSERT_EQUAL(decoded.increment, updater.increment);
    }

    header.entry_count = entryCount;

    vdo_pack_slab_journal_block_header(&header, &block->header);
    struct slab_journal_block_header decoded;
    vdo_unpack_slab_journal_block_header(&block->header, &decoded);
    UDS_ASSERT_EQUAL_BYTES(&header, &decoded, sizeof(decoded));

    VDO_ASSERT_SUCCESS(layer->writer(layer, pbn + i, 1, buffer));
  }

  /*
   * The tail block gets set on slab load; here we set it explicitly
   * to one past the last block we can use. Set the lastSummarized field
   * also so that flushing works.
   */
  journal->tail            = 18;
  journal->last_summarized = journal->tail;
}

/**
 * Implements BlockCondition.
 **/
static bool
shouldBlockVIO(struct vdo_completion *completion,
               void                  *context __attribute__((unused)))
{
  if (!vioTypeIs(completion, VIO_TYPE_SLAB_JOURNAL)
      || (isMetadataRead(completion) != latchRead)) {
    return false;
  }

  // After waiting for a slab journal read, wait for a reference count write.
  // And after the reference count write is latched, stop latching anything.
  if (latchRead) {
    latchRead = false;
  } else {
    clearCompletionEnqueueHooks();
  }

  return true;
}

/**
 * Signal the test thread that the wrapped VIO has made its slab journal entry.
 * This callback is registered in makeWrappedVIO() as the parent callback for
 * adding a slab journal entry.
 **/
static void addEntryComplete(struct vdo_completion *completion)
{
  vdo_finish_completion(completion->parent, completion->result);
  broadcast();
}

/**
 * Construct a data_vio wrapped in a completion.
 *
 * @param [in]  entry          The number for this entry
 * @param [out] wrapperPtr     A pointer to hold the wrapper
 **/
static void makeWrappedVIO(DataVIOWrapper **wrapperPtr)
{
  DataVIOWrapper *wrapper;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, DataVIOWrapper, __func__, &wrapper));
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  vdo_initialize_completion(&wrapper->actionCompletion, vdo,
                            VDO_TEST_COMPLETION);

  struct vdo_completion *completion = &wrapper->dataVIO.decrement_completion;
  vdo_initialize_completion(completion, vdo, VDO_DECREMENT_COMPLETION);
  completion->callback                    = addEntryComplete;
  completion->parent                      = &wrapper->completion;
  struct reference_updater *updater       = &wrapper->dataVIO.decrement_updater;
  wrapper->dataVIO.logical.lbn            = 1;
  wrapper->dataVIO.mapped.pbn             = slab->start + COUNTS_PER_BLOCK;
  updater->operation                      = VDO_JOURNAL_DATA_REMAPPING;
  updater->increment                      = false;
  updater->zpbn.pbn                       = wrapper->dataVIO.mapped.pbn;
  wrapper->dataVIO.recovery_journal_point = (struct journal_point) {
    .sequence_number = 1,
    .entry_count     = 1,
  };

  *wrapperPtr = wrapper;
}

/**
 * The action to add an entry to the journal.
 *
 * @param completion  A wrapper containing the VIO for which to add an entry
 **/
static void addSlabJournalEntryAction(struct vdo_completion *completion)
{
  DataVIOWrapper *wrapper = (DataVIOWrapper *) completion;
  struct data_vio *dataVIO = &wrapper->dataVIO;
  vdo_add_slab_journal_entry(journal, &dataVIO->decrement_completion, &dataVIO->decrement_updater);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Construct a wrapped VIO and launch an action to add an entry for it in
 * the journal.
 *
 * @param entry      The number of the journal entry
 *
 * @return The VIO wrapper to be waited on
 **/
static DataVIOWrapper *performAddEntry(void)
{
  DataVIOWrapper *wrapper;
  makeWrappedVIO(&wrapper);
  VDO_ASSERT_SUCCESS(performAction(addSlabJournalEntryAction,
                                   &wrapper->actionCompletion));
  return wrapper;
}

/**
 * Create reference counts with a known pattern, then set up journal entries.
 * Show that the proper journal mappings are applied to the reference counts
 * while the others are ignored.
 **/
static void testRebuild(void)
{
  initializeReferenceCounts();
  verifyReferences();

  writeSlabJournalBlocks();

  // Setup hook to latch the first metadata write.
  setBlockVIOCompletionEnqueueHook(shouldBlockVIO, false);

  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  launchAction(scrubSlabAction, &completion);

  // Wait for the slab journal to be read during scrubbing.
  struct vio *blockedVIO = getBlockedVIO();

  CU_ASSERT_FALSE(has_waiters(&journal->entry_waiters));
  DataVIOWrapper *vioWrapper = performAddEntry();
  CU_ASSERT_TRUE(has_waiters(&journal->entry_waiters));
  reallyEnqueueVIO(blockedVIO);

  // The in-memory state of the reference count is rebuilt before it is written
  // out to disk, so the in-memory state can be verified.
  blockedVIO = getBlockedVIO();
  verifyReferences();
  reallyEnqueueVIO(blockedVIO);

  VDO_ASSERT_SUCCESS(awaitCompletion(&completion));
  waitForState(&(vioWrapper->completion.complete));
  VDO_ASSERT_SUCCESS(vioWrapper->completion.result);
  UDS_FREE(vioWrapper);

  // The newly added slab journal entry caused the corresponding reference
  // count to change the in-memory state.
  expectedReferences[COUNTS_PER_BLOCK]--;
  verifyReferences();

  // Revert the expected result and load the saved reference counts to ensure
  // slab rebuild wrote it out correctly.
  expectedReferences[COUNTS_PER_BLOCK]++;
  performSuccessfulAction(loadRefCounts);
  verifyReferences();
}

/**********************************************************************/
static CU_TestInfo slabRebuildTests[] = {
  { "rebuild reference counts from slab journal", testRebuild },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo slabRebuildSuite = {
  .name                     = "Rebuild from slab journal (SlabRebuild_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeRebuildTest,
  .cleaner                  = teardownRebuildTest,
  .tests                    = slabRebuildTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &slabRebuildSuite;
}
