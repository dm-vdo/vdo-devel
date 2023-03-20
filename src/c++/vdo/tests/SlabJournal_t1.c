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
#include "permassert.h"

#include "admin-state.h"
#include "encodings.h"
#include "int-map.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"
#include "vio.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "asyncVIO.h"
#include "blockAllocatorUtils.h"
#include "callbackWrappingUtils.h"
#include "completionUtils.h"
#include "intIntMap.h"
#include "latchedCloseUtils.h"
#include "latchUtils.h"
#include "mutexUtils.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef size_t EntryNumber;

typedef struct {
  struct vdo_completion completion;
  EntryNumber           entry;
  bool                  increment;
  struct data_vio       dataVIO;
} DataVIOWrapper;

typedef struct {
  block_count_t           count;
  struct vdo_completion **completions;
} CompletionsWrapper;

/*
 * This test constructs artificial slab journal entries. The journal is eight
 * blocks long and each block can hold 6 full or 8 normal entries. For each
 * trip around the journal, each of the first six blocks will have up to one
 * block map increment, in the entry equal to (sequenceNumber - 1) %
 * journal->size. The remaining 2 blocks will have no block map increments.
 */
enum {
  SLAB_SIZE              = 128,
  SLAB_JOURNAL_SIZE      = 8,
  SLAB_SUMMARY_SIZE      = 4,
  ENTRIES_PER_BLOCK      = 8,
  FULL_ENTRIES_PER_BLOCK = 6,
  VIO_COUNT              = 4,
  PHYSICAL_BLOCKS        = SLAB_SIZE + SLAB_SUMMARY_SIZE,
  FULL_ENTRY_BLOCKS      = FULL_ENTRIES_PER_BLOCK,
  FULL_ENTRIES           = FULL_ENTRIES_PER_BLOCK * FULL_ENTRY_BLOCKS,
  NON_FULL_ENTRY_BLOCKS  = SLAB_JOURNAL_SIZE - FULL_ENTRY_BLOCKS,
  NON_FULL_ENTRIES       = ENTRIES_PER_BLOCK * NON_FULL_ENTRY_BLOCKS,
  TOTAL_JOURNAL_ENTRIES  = FULL_ENTRIES + NON_FULL_ENTRIES,
};

static const TestParameters TEST_PARAMETERS = {
  .slabSize          = SLAB_SIZE,
  .slabCount         = 1,
  .slabJournalBlocks = SLAB_JOURNAL_SIZE,
};

/*
 * A captured encoding of the journal block header created in
 * testHeaderEncoding(). This is used to check that the encoding is
 * platform-independent.
 */
static u8 EXPECTED_BLOCK_HEADER_ENCODING[] =
  {
    0x8a, 0x7a, 0x6a, 0x5a, 0x4a, 0x3a, 0x2a, 0x1a, // head
    0x8b, 0x7b, 0x6b, 0x5b, 0x4b, 0x3b, 0x2b, 0x1b, // sequenceNumber
    0x8c, 0x7c, 0x6c, 0x5c, 0x4c, 0x3c, 0x2c, 0x1c, // recoveryPoint
    0x8d, 0x7d, 0x6d, 0x5d, 0x4d, 0x3d, 0x2d, 0x1d, // nonce
    0x02,                                           // metadataType = SLAB
    0x01,                                           // hasBlockMapIncrements
    0x92, 0x91,                                     // entryCount
  };

static struct slab_depot                *depot;
static struct slab_journal              *journal;
static struct slab_journal_block_header  tailHeader;
static struct vdo_slab                  *slab;

static sequence_number_t                 recoveryJournalLock;
static bool                              commitExpected;
static sequence_number_t                 journalHead;
static sequence_number_t                 expectedJournalHead;
static bool                              journalReaped;
static bool                              releaseFinished;
static IntIntMap                        *expectedHeads;

static sequence_number_t                 referenceSequenceNumber;
static int                               referenceAdjustment;
static EntryNumber                       lastEntry;
static bool                              lastEntryWasIncrement;
static physical_block_number_t           slabSummaryBlockPBN;
static block_count_t                     entriesAdded;
static slab_block_number                 provisional;

/**
 * A WaitCondition to check whether a vio is doing or has just done a slab
 * journal write.
 *
 * @param context  The vio to check
 *
 * @return <code>true</code> if the AsyncVIO is doing (has done) a metadata
 *         write
 **/
static inline bool isSlabJournalWriteCondition(void *context)
{
  struct vio *vio = context;
  if (!isMetadataWrite(&vio->completion)) {
    return false;
  }

  if (onBIOThread()) {
    // We've done the write so signal.
    return true;
  }

  // We're about to do the write, so record what we're updating.
  VDO_ASSERT_SUCCESS(intIntMapPut(expectedHeads,
                                  pbnFromVIO(vio),
                                  journal->head,
                                  true,
                                  NULL,
                                  NULL));
  return false;
}

/**
 * Notify when the callback of a specific PBN has finished.
 *
 * <p>Implements VDOAction.
 **/
static void notifyFinishedRelease(struct vdo_completion *completion)
{
  runSavedCallback(completion);
  signalState(&releaseFinished);
}

/**
 * Implements LatchHook.
 **/
static void vioIsLatched(struct vio *vio)
{
  wrapVIOCallback(vio, notifyFinishedRelease);
}

/**
 * Setup physical and asynchronous layer, then create a slab journal to
 * use the asynchronous layer.
 *
 * @param vioPoolSize  The number of vios for the allocator pool (the rest
 *		       will be reserved for the duration of the test).
 **/
static void slabJournalTestInitialization(block_count_t vioPoolSize)
{
  initializeVDOTest(&TEST_PARAMETERS);
  depot   = vdo->depot;
  slab    = depot->slabs[0];
  journal = slab->journal;

  // Set the threshold policies to be stronger than in production (otherwise
  // the blocking threshold never kicks in for a small slab journal).
  journal->entries_per_block      = ENTRIES_PER_BLOCK;
  journal->full_entries_per_block = FULL_ENTRIES_PER_BLOCK;
  journal->flushing_threshold     = ((SLAB_JOURNAL_SIZE * 2) + 2) / 3;
  journal->blocking_threshold
    = (journal->flushing_threshold + SLAB_JOURNAL_SIZE) / 2;

  slabSummaryBlockPBN = vdo_get_known_partition(&vdo->layout, VDO_SLAB_SUMMARY_PARTITION)->offset;

  // Give refCounts some values so decrement will not underflow it.
  struct slab_config slabConfig = vdo->depot->slab_config;
  for (physical_block_number_t pbn = slab->start;
       pbn < slab->start + slabConfig.data_blocks; pbn++) {
    slab_block_number slabBlockNumber;
    VDO_ASSERT_SUCCESS(slab_block_number_from_pbn(slab, pbn, &slabBlockNumber));
    slab->counters[slabBlockNumber] = 1;
    slab->free_blocks--;
  }

  if (vioPoolSize != BLOCK_ALLOCATOR_VIO_POOL_SIZE) {
    reserveVIOsFromPool(&depot->allocators[0], BLOCK_ALLOCATOR_VIO_POOL_SIZE - vioPoolSize);
  }

  lastEntry       = 0;
  releaseFinished = false;
  VDO_ASSERT_SUCCESS(makeIntIntMap(PHYSICAL_BLOCKS, &expectedHeads));
  initializeLatchUtils(PHYSICAL_BLOCKS,
                       isSlabJournalWriteCondition,
                       NULL,
                       vioIsLatched);
}

/**
 * Initialize a test with default pool sizes.
 **/
static void defaultSlabJournalTestInitialization(void)
{
  slabJournalTestInitialization(BLOCK_ALLOCATOR_VIO_POOL_SIZE);
}

/**
 * Action to check whether the VDO is read-only or the journal is already
 * quiescent and set the layer's stop expectation appropriately.
 **/
static void checkStopExpectation(struct vdo_completion *completion)
{
  int result;
  if (vdo_in_read_only_mode(vdo)) {
    result = VDO_READ_ONLY;
  } else if (vdo_is_state_quiescent(&journal->slab->state)) {
    result = VDO_INVALID_ADMIN_STATE;
  } else {
    result = VDO_SUCCESS;
  }
  setStartStopExpectation(result);

  vdo_finish_completion(completion);
}

/**
 * Free the slab journal along with the physical and asynchronous layer it
 * uses.
 **/
static void slabJournalTestTearDown(void)
{
  clearHooks();

  returnVIOsToPool();

  performSuccessfulAction(checkStopExpectation);
  tearDownVDOTest();
  tearDownLatchUtils();
  freeIntIntMap(&expectedHeads);
}

/**
 * Initialize a vio wrapped in a wrapping completion.
 *
 * @param wrapper  The wrapper to initialize
 **/
static void initializeWrapper(DataVIOWrapper *wrapper)
{
  struct data_vio *dataVIO = &wrapper->dataVIO;
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  vdo_initialize_completion(&dataVIO->vio.completion, vdo, VIO_COMPLETION);
  dataVIO->vio.type                 = VIO_TYPE_DATA;
  vdo_initialize_completion(&dataVIO->decrement_completion, vdo, VDO_DECREMENT_COMPLETION);
  wrapper->dataVIO.mapped.state     = VDO_MAPPING_STATE_UNCOMPRESSED;
  wrapper->dataVIO.new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
}

/**
 * Action to make a provisional reference.
 *
 * <p>Implements VDOAction.
 **/
static void makeProvisionalReference(struct vdo_completion *completion)
{
  slab->counters[provisional] = PROVISIONAL_REFERENCE_COUNT;
  vdo_finish_completion(completion);
}

/**
 * Reset the vio wrapper and the vio it contains.
 *
 * @param wrapper  The wrapper to reset
 * @param entry    The value to set for both the new_mapped and logical
 *                 fields of the vio in the wrapper
 **/
static void resetWrapper(DataVIOWrapper *wrapper, EntryNumber entry)
{
  wrapper->entry       = entry;
  vdo_reset_completion(&wrapper->completion);

  struct data_vio *dataVIO = &wrapper->dataVIO;
  vdo_prepare_completion(&dataVIO->vio.completion,
                         finishParentCallback,
                         finishParentCallback,
                         0,
                         &wrapper->completion);
  vdo_prepare_completion(&dataVIO->decrement_completion,
                         finishParentCallback,
                         finishParentCallback,
                         0,
                         &wrapper->completion);

  physical_block_number_t pbn = (physical_block_number_t) entry + slab->start;
  dataVIO->new_mapped.pbn     = pbn;
  dataVIO->mapped.pbn         = pbn;

  struct reference_updater *incrementer = &dataVIO->increment_updater;
  struct reference_updater *decrementer = &dataVIO->decrement_updater;
  EntryNumber cycleEntry = entry % TOTAL_JOURNAL_ENTRIES;
  if ((cycleEntry % FULL_ENTRIES_PER_BLOCK)
      == (cycleEntry / FULL_ENTRIES_PER_BLOCK)) {
    struct block_map_tree_slot *treeSlot = &dataVIO->tree_lock.tree_slots[1];
    treeSlot->block_map_slot.pbn         = pbn;
    dataVIO->allocation.pbn              = pbn;
    provisional                          = entry;
    dataVIO->tree_lock.height            = 1;
    incrementer->operation               = VDO_JOURNAL_BLOCK_MAP_REMAPPING;
    incrementer->zpbn.pbn                = pbn;
    incrementer->increment               = true;
    wrapper->increment                   = true;
    performSuccessfulActionOnThread(makeProvisionalReference,
                                    slab->allocator->thread_id);
  } else if ((entry % 2) == 0) {
    incrementer->zpbn.pbn  = pbn;
    incrementer->operation = VDO_JOURNAL_DATA_REMAPPING;
    incrementer->increment = true;
    wrapper->increment     = true;
  } else {
    decrementer->zpbn.pbn  = pbn;
    decrementer->operation = VDO_JOURNAL_DATA_REMAPPING;
    decrementer->increment = false;
    wrapper->increment     = false;
  }

  dataVIO->recovery_journal_point = (struct journal_point) {
    .sequence_number = entry + 1,
    .entry_count     = entry % 35,
  };
}

/**
 * Construct a vio wrapped in a completion.
 *
 * @param entry          The number for this entry
 * @param completionPtr  A pointer to hold the wrapper as a completion
 **/
static void makeWrappedVIO(EntryNumber             entry,
                           struct vdo_completion **completionPtr)
{
  DataVIOWrapper *wrapper;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, DataVIOWrapper, __func__, &wrapper));
  initializeWrapper(wrapper);
  resetWrapper(wrapper, entry);
  *completionPtr = &wrapper->completion;
}

/**
 * Implements LockedMethod.
 **/
static bool signalEntryAdded(void *context __attribute__((unused)))
{
  entriesAdded++;
  return true;
}

/**
 * The action to add an entry to the journal.
 *
 * @param completion  A wrapper containing the vio for which to add an entry
 **/
static void addSlabJournalEntryAction(struct vdo_completion *completion)
{
  DataVIOWrapper *wrapper
    = container_of(completion, DataVIOWrapper, completion);
  struct data_vio *dataVIO = &wrapper->dataVIO;
  lastEntryWasIncrement = wrapper->increment;
  if (wrapper->increment) {
    vdo_add_slab_journal_entry(journal, &dataVIO->vio.completion, &dataVIO->increment_updater);
  } else {
    vdo_add_slab_journal_entry(journal,
                               &dataVIO->decrement_completion,
                               &dataVIO->decrement_updater);
  }

  runLocked(signalEntryAdded, NULL);
}

/**
 * The action to add an entry to the journal in rebuild mode.
 *
 * @param completion  A wrapper containing the vio for which to add an entry
 **/
static void
addSlabJournalEntryForRebuildAction(struct vdo_completion *completion)
{
  DataVIOWrapper *wrapper
    = container_of(completion, DataVIOWrapper, completion);
  struct data_vio *dataVIO = &wrapper->dataVIO;
  struct reference_updater *updater = (wrapper->increment
                                       ? &dataVIO->increment_updater
                                       : &dataVIO->decrement_updater);
  bool added
    = vdo_attempt_replay_into_slab_journal(journal,
                                           updater->zpbn.pbn,
                                           updater->operation,
                                           updater->increment,
                                           &dataVIO->recovery_journal_point,
                                           NULL);
  CU_ASSERT(added);
  vdo_finish_completion(completion);
}

/**
 * Construct a wrapped vio and perform an action to add an entry for it in
 * the journal.
 *
 * @param entry      The number of the journal entry
 *
 * @return the number of the next entry
 **/
static EntryNumber performAddEntry(EntryNumber entry)
{
  struct vdo_completion *completion;
  makeWrappedVIO(entry, &completion);
  VDO_ASSERT_SUCCESS(performAction(addSlabJournalEntryAction, completion));
  CU_ASSERT_TRUE(vdo_is_slab_journal_dirty(journal));
  UDS_FREE(completion);
  return (entry + 1);
}

/**
 * Construct a wrapped vio and launch an action to add an entry for it in
 * the journal.
 *
 * @param entry      The number of the journal entry
 *
 * @return The vio wrapper to be waited on
 **/
static struct vdo_completion *launchAddEntry(EntryNumber entry)
{
  struct vdo_completion *completion;
  makeWrappedVIO(entry, &completion);
  launchAction(addSlabJournalEntryAction, completion);
  return completion;
}

/**
 * Add a rebuild-mode entry into the journal.
 *
 * @param entry      The number of the journal entry
 **/
static void addRebuildEntry(EntryNumber entry)
{
  struct vdo_completion *completion;
  makeWrappedVIO(entry, &completion);
  VDO_ASSERT_SUCCESS(performAction(addSlabJournalEntryForRebuildAction,
                                   completion));
  UDS_FREE(completion);
}

/**
 * Free a wrapped completions array.
 *
 * @param wrapped      A pointer to a wrapped completions array
 **/
static void freeWrappedCompletions(CompletionsWrapper *wrapped)
{
  for (unsigned int i = 0; i < wrapped->count; i++) {
    CU_ASSERT_TRUE(wrapped->completions[i]->complete);
    UDS_FREE(wrapped->completions[i]);
  }
  UDS_FREE(wrapped->completions);
}

/**
 * Launch adding a series of entries to the journal.
 *
 * @param start        The number of the first entry to add
 * @param count        The number of entry to add
 * @param wrapped      A pointer to hold the wrapped array of completions
 *                     for the adds
 *
 * @return The number of the next entry
 **/
static EntryNumber addEntries(EntryNumber         start,
                              EntryNumber         count,
                              CompletionsWrapper *wrapped)
{
  struct vdo_completion ***completions = &wrapped->completions;
  wrapped->count = count;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(wrapped->count, struct vdo_completion *,
                                  __func__, completions));
  for (unsigned int i = 0; i < count; i++) {
    (*completions)[i] = launchAddEntry(start + i);
  }
  return start + count;
}

/**
 * Implements WaitCondition.
 **/
static bool checkEntryCount(void *context)
{
  return (entriesAdded >= *((EntryNumber *) context));
}

/**
 * Wait on the completions created in addEntries().
 *
 * @param completionWrapper   The completion wrapper to wait on
 * @param expectStatus        The expected return status
 **/
static void waitForCompletions(CompletionsWrapper *completionWrapper,
                               int                 expectStatus)
{
  for (unsigned int i = 0; i < completionWrapper->count; i++) {
    CU_ASSERT_EQUAL(awaitCompletion(completionWrapper->completions[i]),
                    expectStatus);
  }
}

/**
 * Get the sequence number of the journal block which will contain the
 * specified journal entry.
 *
 * @param entry  The number of a journal entry
 *
 * @return The sequence number of the block to which the specified
 *         entry is expected to be written
 **/
static sequence_number_t sequenceNumberFromEntry(EntryNumber entry)
{
  size_t            cycles         = entry / TOTAL_JOURNAL_ENTRIES;
  sequence_number_t sequenceNumber = (cycles * SLAB_JOURNAL_SIZE) + 1;
  EntryNumber       cycleEntry     = entry % TOTAL_JOURNAL_ENTRIES;
  if (cycleEntry < FULL_ENTRIES) {
    return sequenceNumber + (cycleEntry / FULL_ENTRIES_PER_BLOCK);
  }

  return (sequenceNumber + FULL_ENTRY_BLOCKS
          + ((cycleEntry - FULL_ENTRIES) / ENTRIES_PER_BLOCK));
}

/**
 * Compute the physical block number of the journal block from its sequence
 * number.
 *
 * @param sequenceNumber  The sequence number of the journal block
 *
 * @return The physical block number of the block
 **/
static physical_block_number_t
pbnFromSequenceNumber(sequence_number_t sequenceNumber)
{
  return (slab->journal_origin + (sequenceNumber % journal->size));
}

/**
 * Compute the physical block number of the journal block which will contain
 * the specified journal entry.
 *
 * @param entry  The number of the journal entry
 *
 * @return The physical block number of the block the entry will reside in
 **/
static physical_block_number_t pbnFromEntry(EntryNumber entry)
{
  return pbnFromSequenceNumber(sequenceNumberFromEntry(entry));
}

/**
 * Setup a trap to the committing journal block for a given entry.
 *
 * @param entry  The entry number to block commit for
 *
 * @return The PBN of the journal block which will be blocked
 **/
static physical_block_number_t setupJournalWriteBlocking(EntryNumber entry)
{
  physical_block_number_t pbn = pbnFromEntry(entry);
  setLatch(pbn);
  return pbn;
}

/**
 * Wait for a journal write to be blocked.
 **/
static void waitForJournalWriteBlocked(EntryNumber entry)
{
  waitForLatchedVIO(pbnFromEntry(entry));
}

/**
 * Release the commit of a given block.
 *
 * @param pbn  The physical block number of the vio containing the block
 **/
static void releasePBN(physical_block_number_t pbn)
{
  releaseLatchedVIO(pbn);
  waitForStateAndClear(&releaseFinished);
}

/**
 * Release the commit of a given slab journal block.
 *
 * @param sequenceNumber  The sequence number of the slab journal block
 **/
static void releaseJournalBlock(sequence_number_t sequenceNumber)
{
  releasePBN(pbnFromSequenceNumber(sequenceNumber));
}

/**
 * Commit the journal tail block.
 *
 * <p>Implements VDOAction.
 **/
static void commitJournalTail(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(commitExpected,
                  vdo_release_recovery_journal_lock(journal,
                                                    recoveryJournalLock));
  vdo_finish_completion(completion);
}

/**
 * Perform an action to request that the slab journal release locks on a given
 * recovery journal block by comitting its tail block.
 *
 * @param recoveryLock  The recovery block whose locks should be released
 * @param shouldCommit  Set to <code>true</code> if the tail should lock the
 *                      specified recovery journal block
 **/
static void launchCommitJournalTail(sequence_number_t recoveryLock,
                                    bool              shouldCommit)
{
  recoveryJournalLock = recoveryLock;
  commitExpected      = shouldCommit;
  performSuccessfulAction(commitJournalTail);
}

/**********************************************************************/
static void fetchTailHeader(struct vdo_completion *completion)
{
  tailHeader = journal->tail_header;
  vdo_finish_completion(completion);
}

/**
 * Assert that the journal's append point matches the given parameters.
 *
 * @param blockNumber The expected append point sequence number
 * @param entryCount  The expected append point entry count
 **/
static void assertAppendPoint(sequence_number_t     blockNumber,
                              journal_entry_count_t entryCount)
{
  performSuccessfulAction(fetchTailHeader);
  CU_ASSERT_EQUAL(blockNumber, tailHeader.sequence_number);
  CU_ASSERT_EQUAL(entryCount, tailHeader.entry_count);
}

/**
 * Assert that the journal block's recovery journal point matches the given
 * parameters.
 *
 * @param blockNumber The expected recovery journal point sequence number
 * @param entryCount  The expected recovery journal point entry count
 **/
static void assertRecoveryJournalPoint(sequence_number_t     blockNumber,
                                       journal_entry_count_t entryCount)
{
  performSuccessfulAction(fetchTailHeader);
  struct journal_point recoveryPoint = tailHeader.recovery_point;
  CU_ASSERT_EQUAL(blockNumber, recoveryPoint.sequence_number);
  CU_ASSERT_EQUAL(entryCount * 2 + (lastEntryWasIncrement ? 0 : 1), recoveryPoint.entry_count);
}

/**
 * Verify that the on-disk contents of a journal block are as expected.
 *
 * @param sequenceNumber  The sequence number of the block
 * @param entryCount      The expected number of entries in the block
 **/
static void verifyBlock(sequence_number_t sequenceNumber, uint16_t entryCount)
{
  char buffer[VDO_BLOCK_SIZE];
  physical_block_number_t pbn = pbnFromSequenceNumber(sequenceNumber);
  PhysicalLayer *ramLayer = getSynchronousLayer();
  VDO_ASSERT_SUCCESS(ramLayer->reader(ramLayer, pbn, 1, buffer));

  sequence_number_t expectedHead;
  CU_ASSERT_TRUE(intIntMapGet(expectedHeads, pbn, &expectedHead));

  struct slab_journal_block_header  header;
  struct packed_slab_journal_block *block
    = (struct packed_slab_journal_block *) buffer;
  vdo_unpack_slab_journal_block_header(&block->header, &header);

  CU_ASSERT_EQUAL(expectedHead, header.head);
  CU_ASSERT_EQUAL(sequenceNumber, header.sequence_number);
  CU_ASSERT_EQUAL(depot->allocators[0].nonce, header.nonce);
  CU_ASSERT_EQUAL(entryCount, header.entry_count);

  sequence_number_t zeroBased   = sequenceNumber - 1;
  EntryNumber       baseOffset  = ((zeroBased / SLAB_JOURNAL_SIZE)
                                   * TOTAL_JOURNAL_ENTRIES);
  EntryNumber       cycleOffset = zeroBased % SLAB_JOURNAL_SIZE;
  if (cycleOffset == (SLAB_JOURNAL_SIZE - 1)) {
    baseOffset += (TOTAL_JOURNAL_ENTRIES - ENTRIES_PER_BLOCK);
  } else {
    baseOffset += (FULL_ENTRIES_PER_BLOCK * cycleOffset);
  }

  for (EntryNumber i = 0; i < entryCount; i++) {
    struct slab_journal_entry entry = vdo_decode_slab_journal_entry(block, i);
    EntryNumber expectedOffset = baseOffset + i;
    CU_ASSERT_EQUAL(expectedOffset, entry.sbn);
    if ((expectedOffset % FULL_ENTRIES_PER_BLOCK) == cycleOffset) {
      CU_ASSERT_EQUAL(VDO_JOURNAL_BLOCK_MAP_REMAPPING, entry.operation);
      CU_ASSERT(entry.increment);
    } else {
      CU_ASSERT_EQUAL(VDO_JOURNAL_DATA_REMAPPING, entry.operation);
      CU_ASSERT_EQUAL(((expectedOffset % 2) == 0), entry.increment);
    }
  }
}

/**
 * Call vdo_adjust_slab_journal_block_reference(). This is the action performed
 * by performAdjustment().
 *
 * <p>Implements VDOAction.
 *
 * @param completion The action completion
 **/
static void adjustReference(struct vdo_completion *completion)
{
  vdo_adjust_slab_journal_block_reference(journal, referenceSequenceNumber,
                                          referenceAdjustment);
  vdo_finish_completion(completion);
}

/**
 * Perform an action to call adjustReference() on a slab journal block.
 *
 * @param sequenceNumber  The sequence number of the slab journal block
 * @param adjustment      The amount of adjustment to make
 **/
static void performAdjustment(sequence_number_t sequenceNumber, int adjustment)
{
  referenceSequenceNumber = sequenceNumber;
  referenceAdjustment     = adjustment;
  performSuccessfulAction(adjustReference);
}

/**
 * Check that the entire journal has been committed.
 **/
static void assertJournalCommitted(void)
{
  CU_ASSERT_EQUAL(journal->tail_header.sequence_number, journal->next_commit);
}

/**********************************************************************/
static void checkPacking(slab_block_number sbn, bool increment)
{
  packed_slab_journal_entry packed;
  vdo_pack_slab_journal_entry(&packed, sbn, increment);
  CU_ASSERT_EQUAL(increment, packed.increment);

  u8 *raw = (u8 *) &packed;
  CU_ASSERT_EQUAL(raw[0], packed.offset_low8);
  CU_ASSERT_EQUAL(raw[1], packed.offset_mid8);
  CU_ASSERT_EQUAL(raw[2], (packed.offset_high7 | (increment ? 0x80 : 0)));

  struct slab_journal_entry entry = vdo_unpack_slab_journal_entry(&packed);
  CU_ASSERT_EQUAL(increment, entry.increment);
  CU_ASSERT_EQUAL(VDO_JOURNAL_DATA_REMAPPING, entry.operation);
  CU_ASSERT_EQUAL(sbn, entry.sbn);
}

/**
 * Test the encoding and decoding of slab journal entries.
 **/
static void testEntryEncoding(void)
{
  CU_ASSERT_EQUAL(sizeof(packed_slab_journal_entry), 3);

  checkPacking(0x0, false);
  checkPacking(0x0, true);
  checkPacking(0x123456, true);
  checkPacking(0x7FFFFF, false);
  checkPacking(0x7FFFFF, true);

  // Don't need this, but teardown will fail otherwise.
  defaultSlabJournalTestInitialization();
}

/**
 * Test that packing and unpacking a slab_journal_block_header preserves all
 * fields and always uses little-endian byte order in the on-disk encoding.
 **/
static void testBlockHeaderPacking(void)
{
  struct packed_slab_journal_block_header packed;

  // Catch if the encoding accidentally changes size.
  STATIC_ASSERT(sizeof(packed) == (8 + 8 + 8 + 8 + 1 + 1 + 2));

  /*
   * Declared here in the field order of the packed structure. Eight-byte
   * fields are high-order nibble 1-8 (byte #), low-order nibble A-F (field
   * #). Shorter fields are taken from the sequence 0x91, 0x92, etc, except
   * for the metadata type enum and hasBlockMapIncrements flag.
   */
  struct slab_journal_block_header header = {
    .head                     = 0x1a2a3a4a5a6a7a8a,
    .sequence_number          = 0x1b2b3b4b5b6b7b8b,
    .recovery_point = {
      .sequence_number        = 0x1c2c3c4c5c6c,
      .entry_count            = 0x7c8c,
    },
    .nonce                    = 0x1d2d3d4d5d6d7d8d,
    .metadata_type            = VDO_METADATA_SLAB_JOURNAL,
    .has_block_map_increments = 0x01,
    .entry_count              = 0x9192,
  };

  // Packing and unpacking must preserve all field values.
  vdo_pack_slab_journal_block_header(&header, &packed);
  struct slab_journal_block_header unpacked;
  vdo_unpack_slab_journal_block_header(&packed, &unpacked);

  CU_ASSERT_EQUAL(header.head, unpacked.head);
  CU_ASSERT_EQUAL(header.sequence_number, unpacked.sequence_number);
  CU_ASSERT_EQUAL(header.recovery_point.sequence_number,
                  unpacked.recovery_point.sequence_number);
  CU_ASSERT_EQUAL(header.recovery_point.entry_count,
                  unpacked.recovery_point.entry_count);
  CU_ASSERT_EQUAL(header.metadata_type, unpacked.metadata_type);
  CU_ASSERT_EQUAL(header.has_block_map_increments,
                  unpacked.has_block_map_increments);
  CU_ASSERT_EQUAL(header.entry_count, unpacked.entry_count);

  // Make sure the encoding is in little-endian and hasn't changed accidently.
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_BLOCK_HEADER_ENCODING,
                         (u8 *) &packed, sizeof(packed));

  // Don't need this, but teardown will fail otherwise.
  defaultSlabJournalTestInitialization();
}

/**
 * Work enqueue hook which will fail the test on any slab journal flush.
 *
 * Implements BIOSubmitHook.
 **/
static bool explodeIfAnyFlush(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if (((bio_op(bio) == REQ_OP_FLUSH) || isPreFlush(vio))
      && (vio->type == VIO_TYPE_SLAB_JOURNAL)) {
    CU_FAIL("vdo slab journal flushing unexpectedly!");
  }

  return true;
}

/**
 * Test the slab journal commit policy. Also test that entries cannot be
 * added after the slab journal is closed.
 **/
static void testBasicSlabJournal(void)
{
  defaultSlabJournalTestInitialization();

  CompletionsWrapper wrappedCompletions;
  lastEntry = addEntries(lastEntry, FULL_ENTRIES_PER_BLOCK - 1,
                         &wrappedCompletions);
  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);
  assertAppendPoint(1, FULL_ENTRIES_PER_BLOCK - 1);
  CU_ASSERT_EQUAL(1, journal->next_commit);

  EntryNumber blockedEntry = lastEntry;
  setupJournalWriteBlocking(blockedEntry);
  lastEntry = addEntries(lastEntry, 1, &wrappedCompletions);
  waitForJournalWriteBlocked(blockedEntry);
  releaseJournalBlock(1);
  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);
  assertAppendPoint(2, 0);
  verifyBlock(1, FULL_ENTRIES_PER_BLOCK);

  // Add an entry to the new block and check that it isn't committed.
  blockedEntry = lastEntry;
  setupJournalWriteBlocking(blockedEntry);
  lastEntry = performAddEntry(lastEntry);
  assertAppendPoint(2, 1);

  // Check that asking to release a lock we don't hold does nothing.
  launchCommitJournalTail(lastEntry - 1, false);

  // Check that asking to release the lock we do hold commits the tail.
  launchCommitJournalTail(lastEntry, true);

  // Check that the tail block is committed.
  waitForJournalWriteBlocked(blockedEntry);
  releaseJournalBlock(2);
  assertAppendPoint(3, 0);

  lastEntry = performAddEntry(lastEntry);
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_SUSPENDING);

  // Suspending the journal doesn't write anything
  assertAppendPoint(3, 1);
  // XXX: assert the slab is suspended

  // There is a lock on block 1 (because the first block is locked by every
  // reference block, and we haven't released it).
  CU_ASSERT_EQUAL(1, journal->locks[1].count);
  // Releasing a lock on a suspended journal must not cause reaping to issue a
  // flush via a vio from the pool.
  setBIOSubmitHook(explodeIfAnyFlush);
  performAdjustment(1, -1);
  CU_ASSERT_EQUAL(0, journal->locks[1].count);
  clearBIOSubmitHook();

  // Cannot add entries to a suspended journal.
  lastEntry = addEntries(lastEntry, 1, &wrappedCompletions);
  waitForCompletions(&wrappedCompletions, VDO_INVALID_ADMIN_STATE);
  freeWrappedCompletions(&wrappedCompletions);
  assertAppendPoint(3, 1);

  // Put the lock back so that shutdown won't blow up
  performAdjustment(1, 1);

  // Resume the journal and then save it.
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_RESUMING);
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_SAVING);

  // Quiescing the journal causes tail block to be written out.
  assertAppendPoint(4, 0);
  assertJournalCommitted();

  // Cannot add entries to a quiescent journal.
  lastEntry = addEntries(lastEntry, 1, &wrappedCompletions);
  waitForCompletions(&wrappedCompletions, VDO_INVALID_ADMIN_STATE);
  freeWrappedCompletions(&wrappedCompletions);
  assertAppendPoint(4, 0);
}

/**********************************************************************/
static void verifyRebuiltJournal(void)
{
  assertAppendPoint(journal->size + 1, 0);
  for (sequence_number_t i = 0; i < journal->size; i++) {
    uint16_t expectedEntryCount = FULL_ENTRIES_PER_BLOCK;
    if (i == FULL_ENTRY_BLOCKS) {
      expectedEntryCount = ENTRIES_PER_BLOCK;
    } else if (i == FULL_ENTRY_BLOCKS + 1) {
      expectedEntryCount = 1;
    }
    verifyBlock(i + 1, expectedEntryCount);
  }
}

/**
 * Test that the interface to add entries in rebuild mode works.
 **/
static void testJournalRebuild(void)
{
  defaultSlabJournalTestInitialization();

  // Test that replaying an entire herd of journal entries into an empty slab
  // journal works correctly.
  sequence_number_t head = journal->head;
  for (; lastEntry < FULL_ENTRIES + ENTRIES_PER_BLOCK + 1; lastEntry++) {
    addRebuildEntry(lastEntry);
  }

  // Adding entries during rebuild should have marked the slab as replaying.
  CU_ASSERT_EQUAL(VDO_SLAB_REPLAYING, journal->slab->status);

  // Flush it.
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_RECOVERING);
  CU_ASSERT_EQUAL(journal->head, head);

  // Flushing the journal causes tail block to be written out.
  verifyRebuiltJournal();

  // In lieu of actually restarting, reset the slab as though we had crashed
  // and were coming back online, thereby replaying the entries again.
  journal->slab->status = VDO_SLAB_REBUILT;

  // Assert that re-adding the entries already added has no effect.
  for (EntryNumber i = 0; i < lastEntry; i++) {
    addRebuildEntry(i);
  }

  // Check that the journal is as expected.
  CU_ASSERT_EQUAL(journal->head, head);
  verifyRebuiltJournal();
  CU_ASSERT_NOT_EQUAL(journal->slab->status, VDO_SLAB_REPLAYING);

  // Carefully assert that adding one more entry advances the head.
  addRebuildEntry(lastEntry);
  CU_ASSERT_EQUAL(journal->head, head + 1);
  CU_ASSERT_EQUAL(journal->slab->status, VDO_SLAB_REPLAYING);
}

/**
 * Fill some number of slab journal blocks, assuming that the next entry the
 * journal will make is at the start of a block.
 *
 * @param firstEntry  The entry number of the first entry to make
 * @param blocks      The number of blocks to fill
 * @param wrapped     A pointer to hold the wrapped completions used to make
 *                    the entries. If NULL, the completions will be waited on
 *                    and freed by this method
 *
 * @return The number of the next entry
 **/
static EntryNumber fillBlocks(EntryNumber         firstEntry,
                              block_count_t       blocks,
                              CompletionsWrapper *wrapped)
{
  EntryNumber nextEntry = firstEntry;
  for (block_count_t i = 0; i < blocks; i++) {
    nextEntry += (((nextEntry % TOTAL_JOURNAL_ENTRIES) < FULL_ENTRIES)
                   ? FULL_ENTRIES_PER_BLOCK : ENTRIES_PER_BLOCK);
  }

  CompletionsWrapper wrapper;
  addEntries(firstEntry, nextEntry - firstEntry,
             (wrapped == NULL) ? &wrapper : wrapped);
  if (wrapped == NULL) {
    waitForCompletions(&wrapper, VDO_SUCCESS);
    freeWrappedCompletions(&wrapper);
  }

  return nextEntry;
}

/**
 * Fill some number of slab journal blocks, assuming that the next entry the
 * journal will make is at the start of a block. Wait until the blocks have
 * actually made their entries (in memory).
 *
 * @param firstEntry  The entry number of the first entry to make
 * @param blocks      The number of blocks to fill
 * @param wrapped     A pointer to hold the wrapped completions used to make
 *                    the entries. If NULL, the completions will be waited on
 *                    and freed by this method
 *
 * @return The number of the next entry
 **/
static EntryNumber fillBlocksAndWaitUntilAdded(EntryNumber         firstEntry,
                                               block_count_t       blocks,
                                               CompletionsWrapper *wrapped)
{
  entriesAdded          = 0;
  EntryNumber nextEntry = fillBlocks(firstEntry, blocks, wrapped);
  block_count_t  count  = nextEntry - firstEntry;
  waitForCondition(checkEntryCount, &count);
  return nextEntry;
}

/**
 * Launch action to add entries to fill blocks and block the commit.
 *
 * @param firstEntry  The number of the first entry to make
 * @param blockCount  The number of blocks to fill and block commits for
 *
 * @return The number of the next entry
 **/
static EntryNumber fillAndBlockCommits(EntryNumber   firstEntry,
                                       block_count_t blockCount)
{
  CU_ASSERT_EQUAL(0, journal->tail_header.entry_count);
  sequence_number_t startBlock    = journal->tail_header.sequence_number;
  sequence_number_t journalCommit = journal->next_commit;
  for (block_count_t i = startBlock; i < startBlock + blockCount; i++) {
    setupJournalWriteBlocking(firstEntry);
    EntryNumber nextEntry = fillBlocks(firstEntry, 1, NULL);
    waitForJournalWriteBlocked(firstEntry);
    assertAppendPoint(i + 1, 0);
    CU_ASSERT_EQUAL(journalCommit, journal->next_commit);
    verifyBlock(i, nextEntry - firstEntry);
    firstEntry = nextEntry;
  }

  return firstEntry;
}


/**
 * Load the journal from disk.
 **/
static void loadJournal(void)
{
  /*
   * This tests assumes that a slab journal can be loaded multiple times
   * without affecting the ref_counts. This is not true, but by loading for
   * recovery, the vdo_slab will skip trying to allocate the ref_counts.
   */
  performSuccessfulSlabAction(journal->slab,
                              VDO_ADMIN_STATE_LOADING_FOR_RECOVERY);
}

/**
 * Reset and decode a slab journal from its tail block.
 *
 * @param journal  The slab journal to reset and decode
 **/
static void resetAndDecodeJournal(struct slab_journal *journal)
{
  // Ensure that the journal is quiescent before we try to load it.
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_SUSPENDING);
  journal->head = journal->tail = 0;
  loadJournal();
}

/**
 * BIO submit hook which will fail the test on any slab journal read.
 *
 * Implements BIOSubmitHook.
 **/
static bool explodeIfAnyRead(struct bio *bio)
{
  struct vio *vio = bio->bi_private;
  if ((bio_op(bio) == REQ_OP_READ) && (vio->type == VIO_TYPE_SLAB_JOURNAL)) {
    CU_FAIL("vdo_slab journal read a block it never wrote!");
  }

  return true;
}

/**
 * Test slab journal can be decoded correctly.
 **/
static void testSlabJournalDecode(void)
{
  defaultSlabJournalTestInitialization();
  // Ensure that if we load a slab journal which is empty, no reads are
  // actually issued.
  setBIOSubmitHook(explodeIfAnyRead);
  loadJournal();
  clearBIOSubmitHook();
  // No reads happened if we loaded the slab journal and the hook didn't
  // throw an assertion.

  // Add a single block of journal entries and decode the journal.
  physical_block_number_t blockedPBN = setupJournalWriteBlocking(lastEntry);
  lastEntry = fillBlocks(lastEntry, 1, NULL);
  releasePBN(blockedPBN);

  assertAppendPoint(2, 0);
  CU_ASSERT_EQUAL(2, journal->next_commit);
  resetAndDecodeJournal(journal);
  assertAppendPoint(2, 0);
  CU_ASSERT_EQUAL(journal->head, 1);

  // Add and force out a partial block, then decode the journal.
  EntryNumber blockedEntry = lastEntry;
  blockedPBN = setupJournalWriteBlocking(blockedEntry);

  CompletionsWrapper wrappedCompletions;
  lastEntry = addEntries(lastEntry, (ENTRIES_PER_BLOCK / 2) + 1,
                         &wrappedCompletions);
  launchCommitJournalTail(blockedEntry + 1, true);
  releasePBN(blockedPBN);
  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);

  resetAndDecodeJournal(journal);
  assertAppendPoint(3, 0);
  CU_ASSERT_EQUAL(journal->head, 1);
  assertRecoveryJournalPoint(lastEntry, (lastEntry - 1) % 35);
}

/**
 * Test that the slab journal updates its commit point correctly.
 **/
static void testCommitPoint(void)
{
  defaultSlabJournalTestInitialization();
  // Fill slab journal with entries while blocking the commit to finish.
  lastEntry = fillAndBlockCommits(lastEntry, VIO_COUNT);
  // Releasing the first block should move the commit point.
  releaseJournalBlock(1);
  CU_ASSERT_EQUAL(2, journal->next_commit);

  // Releasing the fourth block should not move the commit point.
  releaseJournalBlock(4);
  CU_ASSERT_EQUAL(2, journal->next_commit);
  CU_ASSERT_FALSE(vdo_is_slab_journal_dirty(journal));

  // Releasing the third block should not move the commit point since the
  // second block is still held up.
  releaseJournalBlock(3);
  CU_ASSERT_EQUAL(2, journal->next_commit);
  CU_ASSERT_FALSE(vdo_is_slab_journal_dirty(journal));

  // Releasing the second block should move the commit point to match the
  // append point since all entries are now committed.
  releaseJournalBlock(2);
  assertAppendPoint(VIO_COUNT + 1, 0);
  assertJournalCommitted();
  CU_ASSERT_FALSE(vdo_is_slab_journal_dirty(journal));

  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_SUSPENDING);
  assertAppendPoint(VIO_COUNT + 1, 0);

  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_RESUMING);
  resetAndDecodeJournal(journal);
  assertAppendPoint(VIO_COUNT + 1, 0);
  CU_ASSERT_EQUAL(journal->head, 1);
}

/**
 * An action to assert that the journal head is as expected. This is done as
 * an action in order to ensure that the head is being checked on the correct
 * thread. Doing so also ensures that the journal is effectively idle assuming
 * this method is called when all other test triggered actions have completed.
 *
 * <p>Implements AsyncAction.
 **/
static void checkJournalHead(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(vdo_get_callback_thread_id(),
		  journal->slab->allocator->thread_id);
  CU_ASSERT_EQUAL(journal->head, expectedJournalHead);
  vdo_finish_completion(completion);
}

/**
 * Assert that the journal head is as expected.
 *
 * @param expected  The expected journal head
 **/
static void assertJournalHead(sequence_number_t expected)
{
  expectedJournalHead = expected;
  performSuccessfulAction(checkJournalHead);
}

/**
 * A locked method to record the journal head and note that the journal has
 * not been reaped since the head was recorded.
 *
 * <p>Implements LockedMethod.
 **/
static bool recordHead(void *context __attribute__((unused)))
{
  journalHead   = journal->head;
  journalReaped = false;
  return false;
}

/**
 * An action to record the journal head.
 *
 * <p>Implements AsyncAction.
 **/
static void recordJournalHead(struct vdo_completion *completion)
{
  CU_ASSERT_EQUAL(vdo_get_callback_thread_id(),
		  journal->slab->allocator->thread_id);
  runLocked(recordHead, NULL);
  vdo_finish_completion(completion);
}

/**
 * A callback finished hook to check if the journal has reaped.
 *
 * <p>Implements FinishedHook.
 **/
static void checkJournalReaped(void)
{
  if ((vdo_get_callback_thread_id() == journal->slab->allocator->thread_id)
      && (journal->head > journalHead)) {
    journalHead = journal->head;
    signalState(&journalReaped);
  }
}

/**
 * Prepare to wait for the journal to reap.
 **/
static void prepareForJournalReapWaiting(void)
{
  performSuccessfulActionOnThread(recordJournalHead,
                                  journal->slab->allocator->thread_id);
  setCallbackFinishedHook(checkJournalReaped);
}

/**
 * An action to save dirty reference blocks.
 **/
static void saveDirtyReferenceBlocksAction(struct vdo_completion *completion)
{
  vdo_save_dirty_reference_blocks(journal->slab);
  vdo_finish_completion(completion);
}

/**
 * Test that the slab journal commits partial blocks correctly and does not
 * reuse those committed partial blocks for new entries.
 **/
static void testPartialBlock(void)
{
  defaultSlabJournalTestInitialization();

  // Committing an empty journal does not change its state.
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_RECOVERING);
  assertAppendPoint(1, 0);
  assertJournalCommitted();

  /*
   * Create a scenario that commits the tail block while the slab journal has
   * a pending vio waiting for a vio to add an entry. Add enough entries to
   * use up all vios in the pool.
   */
  lastEntry = fillAndBlockCommits(lastEntry, VIO_COUNT);

  // Add another block worth of entries which cannot be committed since there
  // are no vios available.
  physical_block_number_t blockedPBNs[2];
  blockedPBNs[0] = setupJournalWriteBlocking(lastEntry);
  lastEntry      = fillBlocks(lastEntry, 1, NULL);

  // Add an entry that would be waiting for a vio.
  CompletionsWrapper wrappedCompletions;
  blockedPBNs[1] = setupJournalWriteBlocking(lastEntry);
  lastEntry      = addEntries(lastEntry, 1, &wrappedCompletions);

  struct vdo_completion *flushCompletion
    = launchSlabAction(journal->slab, VDO_ADMIN_STATE_RECOVERING);
  for (sequence_number_t i = 1; i < VIO_COUNT + 1; i++) {
    releaseJournalBlock(i);
  }

  releasePBN(blockedPBNs[0]);
  releasePBN(blockedPBNs[1]);
  VDO_ASSERT_SUCCESS(awaitCompletion(flushCompletion));
  UDS_FREE(flushCompletion);

  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);
  assertAppendPoint(VIO_COUNT + 3, 0);
  assertJournalCommitted();

  // Flush the dirty reference count blocks so that the entire journal can be
  // reaped.
  prepareForJournalReapWaiting();
  performSuccessfulAction(saveDirtyReferenceBlocksAction);
  waitForState(&journalReaped);
  assertJournalCommitted();
  setCallbackFinishedHook(NULL);

  // Update the entry number to account for unused space in the partial block.
  lastEntry += FULL_ENTRIES_PER_BLOCK - 1;

  // Commit a tail block with just one entry.
  EntryNumber blockedEntry = lastEntry;
  blockedPBNs[0] = setupJournalWriteBlocking(blockedEntry);
  lastEntry = performAddEntry(lastEntry);
  assertAppendPoint(VIO_COUNT + 3, 1);

  launchCommitJournalTail(lastEntry, true);
  waitForJournalWriteBlocked(blockedEntry);
  setLatch(slabSummaryBlockPBN);
  releasePBN(blockedPBNs[0]);
  assertAppendPoint(VIO_COUNT + 4, 0);
  assertJournalCommitted();

  /*
   * Ensure that the slab summary has also updated and that the entire journal
   * has been reaped (which implies that the lock counter for the partial block
   * commit was adjusted correctly).
   */
  prepareForJournalReapWaiting();
  performSuccessfulAction(saveDirtyReferenceBlocksAction);
  releasePBN(slabSummaryBlockPBN);
  waitForState(&journalReaped);
  setCallbackFinishedHook(NULL);
  assertJournalHead(VIO_COUNT + 4);
}

/**
 * Fill the entire slab journal to the blocking threshold, so future writes
 * will be blocked.
 **/
static void fillSlabJournalUntilBlocking(void)
{
  // Fill up to the blocking threshold.
  physical_block_number_t blockedPBNs[2];
  blockedPBNs[0] = setupJournalWriteBlocking(lastEntry);
  lastEntry      = fillBlocks(lastEntry, journal->blocking_threshold - 1, NULL);
  blockedPBNs[1] = setupJournalWriteBlocking(lastEntry);
  lastEntry      = fillBlocks(lastEntry, 1, NULL);
  for (sequence_number_t i = 1; i <= journal->blocking_threshold; i++) {
    performAdjustment(i, 1); // Add an extra lock to prevent reaping
  }

  releasePBN(blockedPBNs[0]);
  releasePBN(blockedPBNs[1]);
  assertJournalHead(1);

  // Test that the expected number of flushes and blocks occurred.
  uint64_t expectedFlushCount
    = (journal->blocking_threshold - journal->flushing_threshold);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->flush_count),
                  expectedFlushCount);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->blocked_count), 0);
}

/**
 * Test that the slab journal reaps unreferenced blocks when adjustments
 * are made to slab journal blocks.
 **/
static void testReaping(void)
{
  defaultSlabJournalTestInitialization();
  fillSlabJournalUntilBlocking();

  uint64_t flushCount = READ_ONCE(journal->events->flush_count);

  // Add entries that will wait for the slab journal to reap.
  CompletionsWrapper      wrappedCompletions;
  EntryNumber             blockedEntry = lastEntry;
  physical_block_number_t blockedPBN   = setupJournalWriteBlocking(blockedEntry);
  lastEntry = fillBlocksAndWaitUntilAdded(lastEntry, 1, &wrappedCompletions);

  // Release the first block to cause the journal to reap it.
  prepareForJournalReapWaiting();
  performSuccessfulAction(saveDirtyReferenceBlocksAction);
  performAdjustment(1, -1);
  waitForState(&journalReaped);
  assertJournalHead(2);

  flushCount += 1;
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->disk_full_count), 0);
  uint64_t blockedCount = lastEntry - blockedEntry;
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->blocked_count),
		  blockedCount);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->flush_count),
		  flushCount);

  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);
  releasePBN(blockedPBN);

  // Add 2 blocks worth of entries that will wait for journal blocks.
  blockedEntry  = lastEntry;
  blockedPBN    = setupJournalWriteBlocking(blockedEntry);
  lastEntry     = fillBlocksAndWaitUntilAdded(lastEntry, 2,
                                              &wrappedCompletions);
  blockedCount += (lastEntry - blockedEntry);
  performSuccessfulAction(saveDirtyReferenceBlocksAction);

  // Unlock the third block. The journal should not reap.
  performAdjustment(4, -1);
  assertJournalHead(2);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->disk_full_count), 0);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->blocked_count), blockedCount);

  // Unlock the second block. The journal should not reap.
  performAdjustment(3, -1);
  assertJournalHead(2);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->disk_full_count), 0);

  // Unlock the first block. The journal should reap.
  prepareForJournalReapWaiting();
  performAdjustment(2, -1);
  waitForState(&journalReaped);
  assertJournalHead(5);

  // Journal was reaped and entries should have been added.
  waitForCompletions(&wrappedCompletions, VDO_SUCCESS);
  freeWrappedCompletions(&wrappedCompletions);
  releasePBN(blockedPBN);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->disk_full_count), 0);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->blocked_count),
		  blockedCount);
  CU_ASSERT_EQUAL(READ_ONCE(journal->events->flush_count),
		  flushCount);
}

// READ-ONLY TESTS

/**
 * Determine whether the journal close is done. Implements
 * ObjectClosednessVerifier.
 **/
static bool journalIsClosed(void *context)
{
  struct slab_journal *journal = context;
  return vdo_is_state_quiescent(&journal->slab->state);
}

/**
 * A wrapper to make drainSlab() take a void *.<p>
 *
 * Implements CloseLauncher.
 **/
static void closeJournalWrapper(void *context, struct vdo_completion *parent)
{
  struct slab_journal *journal = context;

  vdo_start_operation_with_waiter(&journal->slab->state,
                                  VDO_ADMIN_STATE_SAVING,
                                  parent,
                                  initiate_slab_action);
}

/**
 * Release a write blocked by a lack of vio pool entries. Implements
 * BlockedIOReleaser.
 **/
static void releaseBlockedVIOPoolEntry(void *context)
{
  EntryNumber *uncommittedBlockEntry = context;
  for (sequence_number_t i = 1; i < VIO_COUNT + 1; i++) {
    releaseJournalBlock(i);
  }

  // This write won't be launched till it gets a vio pool entry.
  waitForJournalWriteBlocked(*uncommittedBlockEntry);
  releaseJournalBlock(sequenceNumberFromEntry(*uncommittedBlockEntry));
}

/**
 * Test that a slab journal write waiting on a vio pool entry finishes all
 * outstanding IO in read-only mode before finishing its flush completion.
 **/
static void testVIOEntry(void)
{
  // Shrink the vio pool.
  slabJournalTestInitialization(VIO_COUNT);

  // Launch VIO_COUNT blocks and block their commit.
  lastEntry = fillAndBlockCommits(lastEntry, VIO_COUNT);

  // Add another block worth of entries which cannot be committed since there
  // are no vios available.
  EntryNumber uncommittedBlockEntry = lastEntry;
  setupJournalWriteBlocking(uncommittedBlockEntry);
  fillBlocks(lastEntry, 1, NULL);

  // Flush it.
  forceVDOReadOnlyMode();
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = closeJournalWrapper,
    .checker        = journalIsClosed,
    .closeContext   = journal,
    .releaser       = releaseBlockedVIOPoolEntry,
    .releaseContext = &uncommittedBlockEntry,
    .threadID       = journal->slab->allocator->thread_id,
  };
  runLatchedClose(closeInfo, VDO_READ_ONLY);
}

/**
 * Release a latched write. Implements BlockedIOReleaser.
 **/
static void releaseLatchedBlock(void *context __attribute__((unused)))
{
  releaseJournalBlock(1);
}

/**
 * Test that a slab journal waiting on a writing vio finishes all
 * outstanding IO in read-only mode before finishing its flush completion.
 **/
static void testWriting(void)
{
  defaultSlabJournalTestInitialization();

  // Launch and latch a slab journal block write.
  lastEntry = fillAndBlockCommits(lastEntry, 1);

  // Flush it.
  forceVDOReadOnlyMode();
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = closeJournalWrapper,
    .checker        = journalIsClosed,
    .closeContext   = journal,
    .releaser       = releaseLatchedBlock,
    .releaseContext = NULL,
    .threadID       = journal->slab->allocator->thread_id,
  };
  runLatchedClose(closeInfo, VDO_READ_ONLY);
}

/**
 * Release a latched summary write. Implements BlockedIOReleaser.
 **/
static void releaseLatchedSummary(void *context __attribute__((unused)))
{
  releasePBN(slabSummaryBlockPBN);
}

/**
 * Test that a slab journal waiting on a slab summary update finishes
 * all outstanding IO in read-only mode before finishing its flush completion.
 **/
static void testSlabSummary(void)
{
  defaultSlabJournalTestInitialization();

  // Launch and latch a slab journal block write.
  lastEntry = fillAndBlockCommits(lastEntry, 1);

  // Release it and block its slab summary write.
  setLatch(slabSummaryBlockPBN);
  releasePBN(pbnFromSequenceNumber(1));
  waitForLatchedVIO(slabSummaryBlockPBN);

  // Flush it.
  forceVDOReadOnlyMode();
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = closeJournalWrapper,
    .checker        = journalIsClosed,
    .closeContext   = journal,
    .releaser       = releaseLatchedSummary,
    .releaseContext = NULL,
    .threadID       = journal->slab->allocator->thread_id,
  };
  runLatchedClose(closeInfo, VDO_READ_ONLY);
}

/**
 * Check whether a vio is a slab journal flush.
 *
 * <p>Implements BlockCondition.
 **/
static bool
isSlabJournalFlushVIO(struct vdo_completion *completion,
                      void                  *context __attribute__((unused)))
{
  if (!is_vio(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  return (isPreFlush(vio) && (vio->type == VIO_TYPE_SLAB_JOURNAL));
}

/**
 * Release a latched flush.
 *
 * <p>Implements BlockedIOReleaser.
 **/
static void releaseLatchedFlush(void *context __attribute__((unused)))
{
  releaseBlockedVIO();
}

/**
 * Test that a slab journal waiting on reap's flush waits for it in read-only
 * mode before finishing its flush completion.
 **/
static void testReapFlushing(void)
{
  defaultSlabJournalTestInitialization();

  // Write a slab journal block.
  fillBlocks(lastEntry, 1, NULL);

  // Wait for it to be committed by flushing the slab journal.
  performSuccessfulSlabAction(journal->slab, VDO_ADMIN_STATE_RECOVERING);

  // There is a lock on block 1 (because the first block is locked by every
  // reference block, and we haven't released it).
  CU_ASSERT_EQUAL(journal->slab->reference_block_count, journal->locks[1].count);

  // Let go of block 1's locks. It should launch a flush synchronously, which
  // we will block.
  setBlockBIO(isSlabJournalFlushVIO, true);
  performAdjustment(1, -journal->slab->reference_block_count);
  waitForBlockedVIO();

  // Go into read only mode and try closing.
  forceVDOReadOnlyMode();
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = closeJournalWrapper,
    .checker        = journalIsClosed,
    .closeContext   = journal,
    .releaser       = releaseLatchedFlush,
    .releaseContext = NULL,
    .threadID       = journal->slab->allocator->thread_id,
  };
  runLatchedClose(closeInfo, VDO_READ_ONLY);
}

/**********************************************************************/
static CU_TestInfo slabJournalTests[] = {
  { "entry encoding",                     testEntryEncoding      },
  { "header packing",                     testBlockHeaderPacking },
  { "basic",                              testBasicSlabJournal   },
  { "rebuild replay",                     testJournalRebuild     },
  { "decode",                             testSlabJournalDecode  },
  { "commit point",                       testCommitPoint        },
  { "partial block commit",               testPartialBlock       },
  { "reaping",                            testReaping            },
  { "read-only, waiting for vio",          testVIOEntry           },
  { "read-only, while writing",           testWriting            },
  { "read-only, while updating summary",  testSlabSummary        },
  { "read-only, while flushing for reap", testReapFlushing       },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo slabJournalSuite = {
  .name                     = "vdo_slab journal tests (SlabJournal_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = slabJournalTestTearDown,
  .tests                    = slabJournalTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &slabJournalSuite;
}
