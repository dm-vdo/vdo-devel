/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/*
 * A note on the use of physical block numbers in this file.
 *
 * All of the tests which actually add entries to a recovery journal
 * use the convention that the physical and logical block numbers of
 * each entry are the same, and are the absolute 1-based number of the
 * entry in the journal. So the very first entry in the journal will
 * be (1,1), and the next entry will be (2,2). The entry numbers don't
 * wrap even though the journal does.
 */

#include "albtest.h"

#include <linux/prandom.h>

#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "uds-threads.h"

#include "admin-state.h"
#include "num-utils.h"
#include "packed-recovery-journal-block.h"
#include "read-only-notifier.h"
#include "recovery-journal-entry.h"
#include "recovery-journal-format.h"
#include "recovery-journal.h"
#include "vdo.h"

#include "vdoConfig.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "intIntMap.h"
#include "latchUtils.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static const block_count_t     TEST_JOURNAL_TAIL_SIZE   = 8;
static const sequence_number_t SEQUENCE_NUMBER          = 30;
static const nonce_t           TEST_NONCE               = 0x1;
static const block_count_t     TEST_LOGICAL_BLOCKS_USED = 0x123;
static const uint8_t           TEST_RECOVERY_COUNT      = 0xb7;
static const block_count_t     TEST_DATA_BLOCKS_USED    = 0x0001ABCD04030201;

/**
 * A function to decide whether a given journal entry should be an increment
 * or a decrement.
 *
 * @return The selected journal_operation
 **/
typedef enum journal_operation IncrementSelector(void);

typedef size_t EntryNumber;

static struct recovery_journal       *journal;
static struct read_only_notifier     *readOnlyNotifier;
static sequence_number_t              recoverySequenceNumber;
static enum vdo_zone_type             zoneTypeToAdjust;
static int                            adjustment;
static IntIntMap                     *expectedHeads;
static struct journal_point           lastCommittedVIOSeen;
static bool                           noVIOsSeen;
static EntryNumber                    lastEntry;
static struct journal_point           lastAppendPoint;
static enum journal_operation         previousOperation;
static IncrementSelector             *shouldBeIncrement;
static struct thread_config          *threadConfig;
static bool                           injectWriteError;
static const struct admin_state_code *journalState;
static physical_block_number_t        pbnToBlock;
static bool                           priorCommitBlocked;
static bool                           reaping;

typedef struct {
  struct vdo_completion completion;
  EntryNumber           entry;
  struct data_vio       dataVIO;
} DataVIOWrapper;

typedef struct {
  block_count_t           count;
  struct vdo_completion **completions;
} CompletionsWrapper;

enum fakeErrorCodes {
  WRITE_ERROR = -1,
};

/*
 * A captured encoding of the component state version 7.0 resulting from
 * setupEncodeDecodeTest(false). This is used to check that the encoding
 * format hasn't changed and is platform-independent.
 */
static byte EXPECTED_STATE_7_0_ENCODING[] =
  {
    0x02, 0x00, 0x00, 0x00,                         // id: VDO_RECOVERY_JOURNAL
    0x07, 0x00, 0x00, 0x00,                         // majorVersion: 7
    0x00, 0x00, 0x00, 0x00,                         // minorVersion: 0
    0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // size: 0x18 = 8 + 8 + 8
    0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // start: SEQUENCE_NUMBER
    0x23, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // TEST_LOGICAL_BLOCKS_USED
    0x01, 0x02, 0x03, 0x04, 0xCD, 0xAB, 0x01, 0x00, // TEST_DATA_BLOCKS_USED
  };

/*
 * A captured encoding of the journal block header created in
 * testBlockHeaderPacking(). This is used to check that the encoding is
 * platform-independent.
 */
static byte EXPECTED_BLOCK_HEADER_ENCODING[] =
  {
    0x8a, 0x7a, 0x6a, 0x5a, 0x4a, 0x3a, 0x2a, 0x1a, // block_map_head
    0x8b, 0x7b, 0x6b, 0x5b, 0x4b, 0x3b, 0x2b, 0x1b, // slab_journal_head
    0x8c, 0x7c, 0x6c, 0x5c, 0x4c, 0x3c, 0x2c, 0x1c, // sequence_number
    0x8d, 0x7d, 0x6d, 0x5d, 0x4d, 0x3d, 0x2d, 0x1d, // nonce
    0x01,                                           // metadata_type = RECOVERY
    0x92, 0x91,                                     // entry_count
    0x8e, 0x7e, 0x6e, 0x5e, 0x4e, 0x3e, 0x2e, 0x1e, // logical_blocks_used
    0x8f, 0x7f, 0x6f, 0x5f, 0x4f, 0x3f, 0x2f, 0x1f, // block_map_data_blocks
    0x93,                                           // check_byte
    0x94,                                           // recovery_count
  };

/**
 * Record the recovery journal head if this is a recovery journal write
 * and indicated whether or not to latch the VIO.
 *
 * Implements WaitCondition.
 **/
static bool recordRecoveryJournalHead(void *context)
{
  struct vio *vio = context;
  struct bio *bio = vio->bio;
  if ((vio->type != VIO_TYPE_RECOVERY_JOURNAL)
      || (bio_op(bio) == REQ_OP_FLUSH)
      || (bio->bi_vcnt == 0)) {
    return false;
  }

  if (onBIOThread()) {
    if ((injectWriteError) && (pbnFromVIO(vio) % 2) == 1) {
      setVIOResult(vio, WRITE_ERROR);
    }

    // We've done the write, so latch
    return true;
  }

  // We're about to do the write, so record what we're updating
  VDO_ASSERT_SUCCESS(intIntMapPut(expectedHeads,
                                  pbnFromVIO(vio),
                                  journal->block_map_head,
                                  true,
                                  NULL,
                                  NULL));
  return false;
}

/**
 * An IncrementSelector which alternates increments and decrements.
 **/
static enum journal_operation alternateIncrementsAndDecrements(void)
{
  previousOperation = ((previousOperation == VDO_JOURNAL_DATA_INCREMENT)
                       ? VDO_JOURNAL_DATA_DECREMENT
                       : VDO_JOURNAL_DATA_INCREMENT);
  return previousOperation;
}

/**
 * Setup physical and asynchronous layer, then create a recovery journal to
 * use the asynchronous layer.
 **/
static void createLayerAndJournal(void)
{
  TestParameters testParameters = {
    .mappableBlocks = 64,
    .journalBlocks  = 8,
    .noIndexRegion  = true,
  };
  initializeBasicTest(&testParameters);

  threadConfig = makeOneThreadConfig();
  VDO_ASSERT_SUCCESS(vdo_make_read_only_notifier(false,
                                                 threadConfig,
                                                 vdo,
                                                 &readOnlyNotifier));
  block_count_t recovery_journal_size
    = getTestConfig().config.recovery_journal_size;
  VDO_ASSERT_SUCCESS(vdo_decode_recovery_journal(configureRecoveryJournal(),
                                                 TEST_NONCE,
                                                 vdo,
                                                 NULL,
                                                 TEST_RECOVERY_COUNT,
                                                 recovery_journal_size,
                                                 TEST_JOURNAL_TAIL_SIZE,
                                                 readOnlyNotifier,
                                                 threadConfig,
                                                 &journal));
  performSuccessfulRecoveryJournalActionOnJournal(journal,
                                                  VDO_ADMIN_STATE_RESUMING);

  VDO_ASSERT_SUCCESS(makeIntIntMap(journal->size, &expectedHeads));
  lastCommittedVIOSeen.sequence_number = 0;
  lastCommittedVIOSeen.entry_count     = 0;
  noVIOsSeen                           = true;
  lastEntry                            = 0;
  lastAppendPoint.sequence_number      = 0;
  lastAppendPoint.entry_count          = 0;
  previousOperation                    = VDO_JOURNAL_DATA_DECREMENT;
  shouldBeIncrement                    = alternateIncrementsAndDecrements;
  injectWriteError                     = false;
  initializeLatchUtils(journal->size, recordRecoveryJournalHead, NULL, NULL);
  setCallbackFinishedHook(broadcast);
}

/**
 * An action to record the administrative state of the journal.
 **/
static void recordJournalState(struct vdo_completion *completion)
{
  journalState = vdo_get_admin_state_code(&journal->state);
  vdo_complete_completion(completion);
}

/**
 * Free the journal, closing it first if necessary.
 **/
static void freeJournal(void)
{
  performSuccessfulActionOnThread(recordJournalState, journal->thread_id);
  if (!journalState->quiescent) {
    performSuccessfulRecoveryJournalActionOnJournal(journal,
                                                    VDO_ADMIN_STATE_SAVING);
  }
  vdo_free_recovery_journal(UDS_FORGET(journal));
}

/**
 * Free the recovery journal along with the physical and asynchronous layer it
 * uses.
 **/
static void freeLayerAndJournal(void)
{
  freeIntIntMap(&expectedHeads);
  freeJournal();
  tearDownLatchUtils();
  vdo_free_read_only_notifier(UDS_FORGET(readOnlyNotifier));
  vdo_free_thread_config(UDS_FORGET(threadConfig));
  tearDownVDOTest();
}

/**
 * Set up the journal to be in a viable runtime state, for the encode/decode
 * test.
 *
 * @param  closing      Whether to set up a closed journal
 **/
static void setupEncodeDecodeTest(bool closing)
{
  // Change journal before encoding.
  journal->block_map_head          = SEQUENCE_NUMBER;
  journal->slab_journal_head       = SEQUENCE_NUMBER;
  journal->last_write_acknowledged = SEQUENCE_NUMBER + 1;
  journal->tail                  = SEQUENCE_NUMBER + 2;
  journal->logical_blocks_used     = TEST_LOGICAL_BLOCKS_USED;
  journal->block_map_data_blocks    = TEST_DATA_BLOCKS_USED;
  if (closing) {
    performSuccessfulRecoveryJournalActionOnJournal(journal,
                                                    VDO_ADMIN_STATE_RESUMING);
    performSuccessfulRecoveryJournalActionOnJournal(journal,
                                                    VDO_ADMIN_STATE_SAVING);
  }
}

/**
 * Encode and decode the recovery journal.
 *
 * @param checkEncodingBytes  If true, expect that the encoded state
 *                            exactly matches EXPECTED_STATE_7_0_ENCODING
 **/
static void reloadRecoveryJournal(bool checkEncodingBytes)
{
  struct recovery_journal_state_7_0 state
    = vdo_record_recovery_journal(journal);
  struct buffer *buffer;
  VDO_ASSERT_SUCCESS(make_buffer(vdo_get_recovery_journal_encoded_size(),
                                 &buffer));
  VDO_ASSERT_SUCCESS(vdo_encode_recovery_journal_state_7_0(state, buffer));
  freeJournal();

  // Check that the version 7.0 encoding hasn't accidentally been changed,
  // either due to code changes or because of the test platform's endianness.
  if (checkEncodingBytes) {
    CU_ASSERT_EQUAL(sizeof(EXPECTED_STATE_7_0_ENCODING),
                    content_length(buffer));
    UDS_ASSERT_EQUAL_BYTES(EXPECTED_STATE_7_0_ENCODING,
                           get_buffer_contents(buffer),
                           content_length(buffer));
  }

  struct recovery_journal_state_7_0 decoded;
  VDO_ASSERT_SUCCESS(vdo_decode_recovery_journal_state_7_0(buffer, &decoded));
  free_buffer(UDS_FORGET(buffer));

  CU_ASSERT_EQUAL(state.journal_start, decoded.journal_start);
  CU_ASSERT_EQUAL(state.logical_blocks_used, decoded.logical_blocks_used);
  CU_ASSERT_EQUAL(state.block_map_data_blocks, decoded.block_map_data_blocks);

  block_count_t recovery_journal_size
    = getTestConfig().config.recovery_journal_size;
  VDO_ASSERT_SUCCESS(vdo_decode_recovery_journal(decoded,
                                                 TEST_NONCE,
                                                 vdo,
                                                 NULL,
                                                 TEST_RECOVERY_COUNT,
                                                 recovery_journal_size,
                                                 TEST_JOURNAL_TAIL_SIZE,
                                                 readOnlyNotifier,
                                                 threadConfig,
                                                 &journal));
}

/**
 * Verify that the journal reflects the expected saved sequence number.
 *
 * @param expected      The sequence number the journal should start at
 **/
static void verifyDecode(sequence_number_t expected)
{
  // Verify encoded value.
  CU_ASSERT_EQUAL(expected,                 journal->block_map_head);
  CU_ASSERT_EQUAL(expected,                 journal->slab_journal_head);
  CU_ASSERT_EQUAL(expected,                 journal->last_write_acknowledged);
  CU_ASSERT_EQUAL(expected,                 journal->tail);
  CU_ASSERT_EQUAL(TEST_LOGICAL_BLOCKS_USED, journal->logical_blocks_used);
  CU_ASSERT_EQUAL(TEST_DATA_BLOCKS_USED,    journal->block_map_data_blocks);
}

/**
 * Test encoding and decoding of a journal.
 **/
static void testEncodeDecode(void)
{
  // Test an encode/decode during a resize or other such runtime super block
  // save.
  setupEncodeDecodeTest(false);
  reloadRecoveryJournal(true);
  verifyDecode(SEQUENCE_NUMBER);

  // Test an encode/decode during a close.
  setupEncodeDecodeTest(true);
  reloadRecoveryJournal(false);
  verifyDecode(SEQUENCE_NUMBER + 2);
}

/**
 * Pack and unpack the given recovery journal entry, verifying that all
 * information is preserved and that the encoding matches the given array
 * of bytes.
 *
 * @param entry     The recovery journal entry to pack
 * @param expected  The expected raw encoding of the packed entry
 **/
static void checkEntryPacking(const struct recovery_journal_entry *entry,
                              const byte                           expected[])
{
  STATIC_ASSERT_SIZEOF(struct packed_recovery_journal_entry, 11);
  struct packed_recovery_journal_entry packed
    = vdo_pack_recovery_journal_entry(entry);

  // Check that packing and unpacking regenerates the original entry.
  struct recovery_journal_entry unpacked
    = vdo_unpack_recovery_journal_entry(&packed);
  CU_ASSERT_EQUAL(entry->operation,     unpacked.operation);
  CU_ASSERT_EQUAL(entry->mapping.pbn,   unpacked.mapping.pbn);
  CU_ASSERT_EQUAL(entry->mapping.state, unpacked.mapping.state);
  CU_ASSERT_EQUAL(entry->slot.pbn,      unpacked.slot.pbn);
  CU_ASSERT_EQUAL(entry->slot.slot,     unpacked.slot.slot);

  // Spot-check that the packed and unpacked fields correspond, and that we're
  // not accidently swapping the mapping PBN with the slot PBN.
  CU_ASSERT_EQUAL(entry->operation,    packed.operation);
  CU_ASSERT_EQUAL(entry->slot.slot,    (packed.slot_low
                                        | (packed.slot_high << 6)));
  CU_ASSERT_EQUAL(entry->mapping.pbn,
                  vdo_unpack_block_map_entry(&packed.block_map_entry).pbn);

  // Check that packing generates the specified encoding.
  UDS_ASSERT_EQUAL_BYTES(expected, (byte *) &packed, sizeof(packed));
}

/**
 * Test that a packed_recovery_journal_entry always maps to the correct
 * bitfields and little-endian byte order in the on-disk encoding.
 **/
static void testEntryPacking(void)
{
  byte expected[sizeof(struct packed_recovery_journal_entry)];
  memset(expected, 0, sizeof(expected));
  struct recovery_journal_entry entry;

  // Check all operation encodings.
  for (unsigned operation = 0; operation <= VDO_JOURNAL_BLOCK_MAP_INCREMENT;
       operation++) {
    expected[0] = operation;
    entry = (struct recovery_journal_entry) { .operation = operation };
    checkEntryPacking(&entry, expected);
  };

  memset(expected, 0, sizeof(expected));

  // Check all 10-bit slot encodings.
  for (unsigned int slot = 0; slot < (1 << 10); slot++) {
    entry = (struct recovery_journal_entry) {
      .slot = { .slot = slot }
    };
    expected[0] = (slot << 2) & 0xFF;
    expected[1] = (slot >> 6);
    checkEntryPacking(&entry, expected);
  }

  memset(expected, 0, sizeof(expected));

  // The maximum legal PBN must be representable by the packed encoding.
  entry = (struct recovery_journal_entry) {
    .slot = { .pbn = MAXIMUM_VDO_PHYSICAL_BLOCKS - 1 }
  };
  byte highNibble = (entry.slot.pbn >> 32);
  expected[1] = (highNibble << 4);
  put_unaligned_le32(entry.slot.pbn & 0xFFFFFFFF, &expected[2]);
  checkEntryPacking(&entry, expected);

  // Spot-check that the slot PBN encoding is in little-endian layout
  // using a PBN with distinct nibbles.
  entry = (struct recovery_journal_entry) {
    .slot = { .pbn = 0xABCDE6789 }
  };
  expected[1] = 0xA0;
  put_unaligned_le32(entry.slot.pbn & 0xFFFFFFFF, &expected[2]);
  checkEntryPacking(&entry, expected);

  memset(expected, 0, sizeof(expected));

  // The block map entry encoding is tested for correctness elsewhere,
  // so just spot-check that it's used with a random encoding.
  struct packed_recovery_journal_entry packed;
  prandom_bytes((byte *) &packed, sizeof(packed));
  entry = vdo_unpack_recovery_journal_entry(&packed);
  checkEntryPacking(&entry, (byte *) &packed);
}

/**
 * Test that packing and unpacking a packed_journal_header preserves all fields
 * and always uses little-endian byte order in the on-disk encoding.
 **/
static void testBlockHeaderPacking(void)
{
  struct packed_journal_header packed;

  // Catch if the encoding accidentally changes size.
  STATIC_ASSERT(sizeof(packed) == (8 + 8 + 8 + 8 + 1 + 2 + 8 + 8 + 1 + 1));

  /*
   * Declared here in the field order of the packed structure. Eight-byte
   * fields are high-order nibble 1-8 (byte #), low-order nibble A-F (field
   * #). Shorter fields are taken from the sequence 0x91, 0x92, etc, except
   * for the metadata type enum.
   */
  struct recovery_block_header header = {
    .block_map_head        = 0x1a2a3a4a5a6a7a8a,
    .slab_journal_head     = 0x1b2b3b4b5b6b7b8b,
    .sequence_number       = 0x1c2c3c4c5c6c7c8c,
    .nonce                 = 0x1d2d3d4d5d6d7d8d,
    .metadata_type         = VDO_METADATA_RECOVERY_JOURNAL,
    .entry_count           = 0x9192,
    .logical_blocks_used   = 0x1e2e3e4e5e6e7e8e,
    .block_map_data_blocks = 0x1f2f3f4f5f6f7f8f,
    .check_byte            = 0x93,
    .recovery_count        = 0x94,
  };

  // Packing and unpacking must preserve all field values.
  vdo_pack_recovery_block_header(&header, &packed);
  struct recovery_block_header unpacked;
  vdo_unpack_recovery_block_header(&packed, &unpacked);

  CU_ASSERT_EQUAL(header.block_map_head, unpacked.block_map_head);
  CU_ASSERT_EQUAL(header.slab_journal_head, unpacked.slab_journal_head);
  CU_ASSERT_EQUAL(header.sequence_number, unpacked.sequence_number);
  CU_ASSERT_EQUAL(header.nonce, unpacked.nonce);
  CU_ASSERT_EQUAL(header.metadata_type, unpacked.metadata_type);
  CU_ASSERT_EQUAL(header.entry_count, unpacked.entry_count);
  CU_ASSERT_EQUAL(header.logical_blocks_used, unpacked.logical_blocks_used);
  CU_ASSERT_EQUAL(header.block_map_data_blocks,
                  unpacked.block_map_data_blocks);
  CU_ASSERT_EQUAL(header.check_byte, unpacked.check_byte);
  CU_ASSERT_EQUAL(header.recovery_count, unpacked.recovery_count);

  // Make sure the encoding is in little-endian and hasn't changed accidently.
  UDS_ASSERT_EQUAL_BYTES(EXPECTED_BLOCK_HEADER_ENCODING,
                         &packed, sizeof(packed));
}

/**
 * Initialize a VIO wrapped in a wrapping completion.
 *
 * @param wrapper  The wrapper to initialize
 **/
static void initializeWrapper(DataVIOWrapper *wrapper)
{
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  vdo_initialize_completion(data_vio_as_completion(&wrapper->dataVIO), vdo,
                            VIO_COMPLETION);
  data_vio_as_vio(&wrapper->dataVIO)->type = VIO_TYPE_DATA;
  wrapper->dataVIO.mapped.state         = VDO_MAPPING_STATE_UNCOMPRESSED;
  wrapper->dataVIO.new_mapped.state     = VDO_MAPPING_STATE_UNCOMPRESSED;
}

/**
 * Verify that VIOs are released from the journal in the order in which they
 * made their entries.
 *
 * @param completion  The VIO which just finished committing (as a completion)
 **/
static void journalEntryCallback(struct vdo_completion *completion)
{
  if (completion->result == VDO_SUCCESS) {
    struct data_vio *dataVIO = as_data_vio(completion);
    if (noVIOsSeen) {
      noVIOsSeen = false;
    } else {
      bool before = vdo_before_journal_point(&lastCommittedVIOSeen,
                                             &dataVIO->recovery_journal_point);
      CU_ASSERT_TRUE(before);
    }

    lastCommittedVIOSeen = dataVIO->recovery_journal_point;
  }

  vdo_finish_completion_parent_callback(completion);
}

/**
 * Reset the VIO wrapper and the VIO it contains.
 *
 * @param wrapper  The wrapper to reset
 * @param entry    The value to set for both the new_mapped and logical
 *                 fields of the VIO in the wrapper (see note at top of file)
 **/
static void resetWrapper(DataVIOWrapper *wrapper, EntryNumber entry)
{
  vdo_reset_completion(&wrapper->completion);
  struct vdo_completion *completion
    = data_vio_as_completion(&wrapper->dataVIO);
  vdo_reset_completion(completion);
  completion->callback            = journalEntryCallback;
  completion->parent              = &wrapper->completion;
  wrapper->entry                  = entry;
  wrapper->dataVIO.new_mapped.pbn = (physical_block_number_t) entry;
  wrapper->dataVIO.tree_lock.tree_slots[0].block_map_slot.pbn
    = (physical_block_number_t) entry;
}

/**
 * Construct a VIO wrapped in a wrapping completion.
 *
 * @param entry          The value to set for both the new_mapped and
 *                       logical fields of the VIO in the wrapper (see
 *                       note at top of file)
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
 * Extract a data_vio from its wrapper.
 *
 * @param completion  The wrapper containing the data_vio
 *
 * @return The unwrapped data_vio
 **/
static inline struct data_vio *
dataVIOFromWrapper(struct vdo_completion *completion)
{
  return &(((DataVIOWrapper *) completion)->dataVIO);
}

/**
 * Implements LockedMethod.
 **/
static bool recordAppendPoint(void *context __attribute__((unused)))
{
  /*
   * It isn't safe to look at the VIO here since it may have been freed
   * before vdo_add_recovery_journal_entry() returned, similarly, the journal
   * may have been torn down, so if closeRequested has been set, we're done.
   */
  if (vdo_is_state_quiescent(&journal->state)
      || vdo_are_equivalent_journal_points(&journal->append_point,
                                           &lastAppendPoint)) {
    return false;
  }

  lastAppendPoint = journal->append_point;
  return true;
}

/**
 * The action to add an entry to the journal.
 *
 * @param completion A wrapper containing the VIO for which to add an entry
 **/
static void addJournalEntry(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = dataVIOFromWrapper(completion);
  vdo_set_up_reference_operation_with_lock(shouldBeIncrement(),
                                           dataVIO->new_mapped.pbn,
                                           dataVIO->new_mapped.state,
                                           NULL,
                                           &dataVIO->operation);
  vdo_add_recovery_journal_entry(journal, dataVIO);
  runLocked(recordAppendPoint, NULL);
}

/**
 * Implements WaitCondition.
 **/
static bool checkAppendPoint(void *context)
{
  return !vdo_before_journal_point(&lastAppendPoint,
                                   (struct journal_point *) context);
}

/**
 * Wait for the append point to reach a certain point.
 *
 * @param sequenceNumber  The sequence number to wait for
 * @param entryCount      The entry count to wait for
 **/
static void waitForAppendPoint(sequence_number_t sequenceNumber,
                               journal_entry_count_t entryCount)
{
  struct journal_point desiredAppendPoint = {
    .sequence_number = sequenceNumber,
    .entry_count     = entryCount,
  };
  waitForCondition(checkAppendPoint, &desiredAppendPoint);
}

/**
 * Construct a wrapped VIO and launch an action to add an entry for it in
 * the journal.
 *
 * @param entry  The number of the journal entry (see note top of file)
 *
 * @return The VIO wrapper to be waited on
 **/
static struct vdo_completion *addEntry(EntryNumber entry)
{
  struct vdo_completion *completion;
  makeWrappedVIO(entry, &completion);
  struct journal_point appendPoint = journal->append_point;
  vdo_advance_journal_point(&appendPoint, journal->entries_per_block);
  launchAction(addJournalEntry, completion);
  waitForAppendPoint(appendPoint.sequence_number, appendPoint.entry_count);
  return completion;
}

/**
 * Launch adding a series of entries to the journal.
 *
 * @param start        The number of the first entry to add (see note
 *                     at top of file)
 * @param count        The number of entries to add
 * @param wrapped      A pointer to hold the wrapped array of completions
 *                     for the adds
 *
 * @return The number of the next entry (i.e. start + count)
 **/
static EntryNumber launchAddEntries(EntryNumber         start,
                                    block_count_t       count,
                                    CompletionsWrapper *wrapped)
{
  // Local variable is just to avoid a bogus type error from UDS_ALLOCATE.
  struct vdo_completion ***completions = &wrapped->completions;
  wrapped->count = count;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(count, struct vdo_completion *, __func__,
                                  completions));
  EntryNumber nextEntry = start;
  for (unsigned int i = 0; i < count; i++) {
    wrapped->completions[i] = addEntry(nextEntry++);
  }
  return nextEntry;
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
 * Wait on the completions created in launchAddEntries().
 *
 * @param completions  The completions to wait on
 * @param count        The number of completions to wait on
 **/
static void waitForCompletions(struct vdo_completion **completions,
                               block_count_t           count)
{
  for (unsigned int i = 0; i < count; i++) {
    VDO_ASSERT_SUCCESS(awaitCompletion(completions[i]));
  }
}

/**
 * Get the sequence number of the journal block which will contain the
 * specified journal entry.
 *
 * @param entry  The number of a journal entry (see note at top of file)
 *
 * @return The sequence number of the block to which the specified
 *         entry is expected to be written
 **/
static sequence_number_t sequenceNumberFromEntry(EntryNumber entry)
{
  return (((entry - 1) / journal->entries_per_block) + 1);
}

/**
 * Determine whether a given entry should be an increment.
 *
 * @param sequenceNumber  The sequence number of a journal block
 * @param entry           The entry offset into the block
 *
 * @return <code>true</code> if the entry should be an increment
 **/
static bool isIncrementEntry(sequence_number_t sequenceNumber,
                             EntryNumber entry)
{
  return (((sequenceNumber + entry) % 2) == 1);
}

/**
 * Copy a recovery journal block out of the RAM layer.
 *
 * @param sequenceNumber  The sequence number of the desired block
 *
 * @return A copy of the desired block (it is the caller's responsibility
 *         to free it)
 **/
static void *getJournalBlockFromLayer(sequence_number_t sequenceNumber)
{
  char *block;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(VDO_BLOCK_SIZE, char, __func__, &block));
  physical_block_number_t pbn = sequenceNumber % journal->size;
  PhysicalLayer *ramLayer = getSynchronousLayer();
  VDO_ASSERT_SUCCESS(ramLayer->reader(ramLayer, pbn, 1, block));
  struct packed_journal_header *header
    = (struct packed_journal_header *) block;
  sequence_number_t expectedHead;
  CU_ASSERT_TRUE(intIntMapGet(expectedHeads, pbn, &expectedHead));
  CU_ASSERT_EQUAL(expectedHead, __le64_to_cpu(header->block_map_head));
  CU_ASSERT_EQUAL(expectedHead, __le64_to_cpu(header->slab_journal_head));
  return block;
}

/**
 * Action to check whether the journal is reaping.
 **/
static void checkReaping(struct vdo_completion *completion)
{
  reaping = journal->reaping;
  vdo_complete_completion(completion);
}

/**
 * Poll the journal until it is not reaping.
 **/
static void pollUntilNotReaping(void)
{
  for (reaping = true; reaping; ) {
    performSuccessfulAction(checkReaping);
  }
}

/**
 * Verify that the on-disk contents of a journal block are as expected.
 *
 * @param sequenceNumber  The sequence number of the block
 * @param entryCount      The expected number of entries in the block
 **/
static void verifyBlock(sequence_number_t sequenceNumber, uint16_t entryCount)
{
  pollUntilNotReaping();
  struct packed_journal_header *packedHeader
    = getJournalBlockFromLayer(sequenceNumber);
  struct recovery_block_header header;
  vdo_unpack_recovery_block_header(packedHeader, &header);
  CU_ASSERT_EQUAL(sequenceNumber, header.sequence_number);
  CU_ASSERT_EQUAL(journal->nonce, header.nonce);
  CU_ASSERT_EQUAL(entryCount, header.entry_count);
  CU_ASSERT_EQUAL(TEST_RECOVERY_COUNT, header.recovery_count);

  struct packed_journal_sector *sector
    = (struct packed_journal_sector *) packedHeader;
  for (EntryNumber i = 0; i < entryCount; i++) {
    EntryNumber sectorEntryNumber = i % RECOVERY_JOURNAL_ENTRIES_PER_SECTOR;
    if (sectorEntryNumber == 0) {
      sector = (struct packed_journal_sector *) ((char *) sector
                                                 + VDO_SECTOR_SIZE);
      CU_ASSERT_EQUAL(header.check_byte, sector->check_byte);
      CU_ASSERT_EQUAL(TEST_RECOVERY_COUNT, sector->recovery_count);

      journal_entry_count_t entriesRemaining = entryCount - i;
      journal_entry_count_t expectedEntries
        = min(entriesRemaining,
              (journal_entry_count_t) RECOVERY_JOURNAL_ENTRIES_PER_SECTOR);
      CU_ASSERT_EQUAL(sector->entry_count, expectedEntries);
    }

    EntryNumber entryNumber
      = i + ((sequenceNumber - 1) * journal->entries_per_block) + 1;
    struct recovery_journal_entry entry
      = vdo_unpack_recovery_journal_entry(&sector->entries[sectorEntryNumber]);
    CU_ASSERT_EQUAL(isIncrementEntry(sequenceNumber, i),
                    vdo_is_journal_increment_operation(entry.operation));
    CU_ASSERT_EQUAL(entryNumber, entry.slot.pbn);
    CU_ASSERT_EQUAL(entryNumber, entry.mapping.pbn);
  }
  UDS_FREE(packedHeader);
}

/**
 * Verify that a range of full journal blocks on disk meet expectations.
 *
 * @param start  The sequence number of the first block to check
 * @param end    The sequence number of the last block to check
 **/
static void verifyFullBlocks(sequence_number_t start, sequence_number_t end)
{
  for (sequence_number_t i = start; i <= end; i++) {
    verifyBlock(i, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK);
  }
}

/**
 * Assert that the last VIO whose entry was committed by the journal had the
 * given recovery journal point.
 *
 * @param blockNumber The expected commit point sequence number
 * @param entryCount  the expected commit point entry count
 **/
static void assertLastVIOCommitted(sequence_number_t     blockNumber,
                                   journal_entry_count_t entryCount)
{
  CU_ASSERT_EQUAL(blockNumber, lastCommittedVIOSeen.sequence_number);
  CU_ASSERT_EQUAL(entryCount , lastCommittedVIOSeen.entry_count);
}

/**
 * Add a single entry to the journal and wait until the action
 * completes (this will be when it is committed to disk if the write
 * policy is synchronous, and when the entry has been added in-memory
 * if the write policy is asynchronous).
 *
 * @param entry  The number of a journal entry (see note at top of file)
 **/
static void addOneEntry(EntryNumber entry)
{
  struct vdo_completion *completion = addEntry(entry);
  VDO_ASSERT_SUCCESS(awaitCompletion(completion));

  sequence_number_t   expectedSequenceNumber = sequenceNumberFromEntry(entry);
  struct data_vio    *dataVIO                = dataVIOFromWrapper(completion);
  CU_ASSERT_EQUAL(expectedSequenceNumber,
                  dataVIO->recovery_journal_point.sequence_number);
  UDS_FREE(completion);
}

/**
 * Add a single entry to the journal and wait for it to be committed.
 * Verify that the journal block in which it was written meets expectations.
 *
 * @param entry  The number of a journal entry (see note at top of file)
 **/
static void commitOneEntry(EntryNumber entry)
{
  addOneEntry(entry);
  sequence_number_t expectedSequenceNumber = sequenceNumberFromEntry(entry);
  uint16_t expectedEntryCount = entry % journal->entries_per_block;
  if (expectedEntryCount == 0) {
    expectedEntryCount = journal->entries_per_block;
  }
  verifyBlock(expectedSequenceNumber, expectedEntryCount);
}

/**
 * Add a series of entries to the journal and wait for each to be committed.
 * Verify after each that the journal block in which it was written meets
 * expectations.
 *
 * @param start        The number of the first entry to add (see note
 *                     at top of file)
 * @param end          The number of the last entry to add
 *
 * @return The number of the next entry (i.e. end + 1)
 **/
static EntryNumber commitEntries(EntryNumber start, EntryNumber end)
{
  EntryNumber entry;
  for (entry = start; entry <= end; entry++) {
    commitOneEntry(entry);

    sequence_number_t expectedSequenceNumber = sequenceNumberFromEntry(entry);
    uint16_t          expectedEntryCount     = entry
      % journal->entries_per_block;
    if (expectedEntryCount == 0) {
      expectedSequenceNumber++;
    }
    waitForAppendPoint(expectedSequenceNumber, expectedEntryCount);
  }
  return entry;
}

/**
 * Compute the physical block number of the journal block which will contain
 * the specified journal entry.
 *
 * @param entry  The number of the journal entry (see note at top of file)
 *
 * @return The physical block number of the block the entry will reside in
 **/
static physical_block_number_t pbnFromEntry(EntryNumber entry)
{
  return (sequenceNumberFromEntry(entry) % journal->size);
}

/**
 * Check whether a given VIOLatch is a blocked commit which will prevent the
 * current commit from being issued.
 *
 * Implements LatchExaminer
 **/
static bool isPriorBlockedCommit(VIOLatch *latch)
{
  if (latch->vio == NULL) {
    return false;
  }

  if (journal->tail < pbnToBlock) {
    priorCommitBlocked = ((journal->tail <= latch->pbn)
                          && (latch->pbn <= pbnToBlock));
  } else {
    priorCommitBlocked = ((journal->tail <= latch->pbn)
                          || (latch->pbn <= pbnToBlock));
  }

  return priorCommitBlocked;
}

/**
 * Wait until a given entry's journal write is blocked.
 *
 * @param entry  The number of the entry
 **/
static void waitForBlockedCommit(EntryNumber entry)
{
  pbnToBlock         = pbnFromEntry(entry);
  priorCommitBlocked = false;
  examineLatches(isPriorBlockedCommit);
  if (!priorCommitBlocked) {
    waitForLatchedVIO(pbnToBlock);
  }
}

/**
 * Implements WaitCondition.
 **/
static bool checkCommitPoint(void *context)
{
  return !vdo_before_journal_point(&lastCommittedVIOSeen,
                                   (struct journal_point *) context);
}

/**
 * Wait until the journal has committed up to and including the journal
 * point described by the given sequence number and entry count.
 *
 * @param blockNumber The sequence number of the wait point
 * @param entryCount  the entry count of the wait point
 **/
static void waitForJournalCommit(sequence_number_t     blockNumber,
                                 journal_entry_count_t entryCount)
{
  struct journal_point waitPoint = {
    .sequence_number = blockNumber,
    .entry_count     = entryCount,
  };
  waitForCondition(checkCommitPoint, &waitPoint);
}

/**
 * Release the commit of an entry's journal write.
 *
 * @param entry  The entry
 **/
static void releaseCommit(EntryNumber entry)
{
  releaseLatchedVIO(pbnFromEntry(entry));
}

/**
 * Release all blocked journal commits. No further commits will be blocked
 * without explicitly requesting them.
 **/
static void releaseAllCommits(void)
{
  for (physical_block_number_t pbn = 0; pbn < journal->size; pbn++) {
    clearLatch(pbn);
  }
}

/**
 * Block one journal commit.
 *
 * @param entry  The number of the entry whose journal block should be
 *               blocked (see note at top of file)
 **/
static void blockCommit(EntryNumber entry)
{
  setLatch(pbnFromEntry(entry));
}

/**
 * Block all journal commits.
 **/
static void blockAllCommits(void)
{
  for (physical_block_number_t pbn = 0; pbn < journal->size; pbn++) {
    setLatch(pbn);
  }
}

/**
 * Prepare to block a journal commit, then launch a series of entry
 * additions and wait until the ensuing journal commit has been blocked.
 *
 * @param start        The number of the first entry to add (see note
 *                     at top of file)
 * @param count        The number of entries to add
 * @param wrapped      A pointer to hold the wrapped array of completions
 *                     for the adds
 *
 * @return The number of the next entry (i.e. end + 1)
 **/
static EntryNumber launchAddWithBlockedCommit(EntryNumber         start,
                                              block_count_t       count,
                                              CompletionsWrapper *wrapped)
{
  blockCommit(start);
  EntryNumber nextEntry = launchAddEntries(start, count, wrapped);
  waitForBlockedCommit(start);
  return nextEntry;
}

/**
 * Release a blocked commit and then wait on the add completions made in
 * launchAddWithBlockedCommit().
 *
 * @param completions  The completions to wait on
 * @param blocked      The entry whose block commit was blocked
 * @param count        The number of completions to wait on
 **/
static void releaseAndWaitForCompletions(struct vdo_completion **completions,
                                         EntryNumber             blocked,
                                         block_count_t           count)
{
  releaseCommit(blocked);
  waitForCompletions(completions, count);
}

/**
 * VDOAction to call adjustRecoveryBlockReference(). Implements VDOAction.
 * This is the action performed by performAdjustment().
 *
 * @param completion The action completion
 **/
static void adjustReference(struct vdo_completion *completion)
{
  if (adjustment == 1) {
    vdo_acquire_recovery_journal_block_reference(journal,
                                                 recoverySequenceNumber,
                                                 zoneTypeToAdjust, 0);
  } else {
    vdo_release_recovery_journal_block_reference(journal,
                                                 recoverySequenceNumber,
                                                 zoneTypeToAdjust, 0);
  }

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Add a reference to a journal block.
 *
 * @param sequenceNumber  The sequence number of the block gaining the
 *                        reference
 * @param zoneType        The type of zone acquiring the reference
 **/
static void addReference(sequence_number_t  sequenceNumber,
                         enum vdo_zone_type zoneType)
{
  adjustment             = 1;
  recoverySequenceNumber = sequenceNumber;
  zoneTypeToAdjust       = zoneType;
  performSuccessfulAction(adjustReference);
}

/**
 * Remove a reference to a journal block.
 *
 * @param sequenceNumber  The sequence number of the block losing the
 *                        reference
 * @param zoneType        The type of zone releasing the reference
 **/
static void removeReference(sequence_number_t  sequenceNumber,
                            enum vdo_zone_type zoneType)
{
  adjustment             = -1;
  recoverySequenceNumber = sequenceNumber;
  zoneTypeToAdjust       = zoneType;
  performSuccessfulAction(adjustReference);
}

/**
 * Simulate doing the complete set of block map updates for a single
 * journal block.
 *
 * @param blockNumber   The number of the journal block getting the updates
 * @param expectedHead  The expected head of the journal after the updates
 **/
static void simulateUpdatesForBlock(sequence_number_t blockNumber,
                                    sequence_number_t expectedHead)
{
  // Pretend all of the requests within the given block were to the same
  // slab journal and block map page.
  addReference(blockNumber, VDO_ZONE_TYPE_PHYSICAL);
  addReference(blockNumber, VDO_ZONE_TYPE_LOGICAL);
  for (int i = 0; i < journal->entries_per_block; i++) {
    // Making a block map entry releases a lock on increment.
    if (isIncrementEntry(blockNumber, i)) {
      vdo_release_journal_per_entry_lock_from_other_zone(journal, blockNumber);
    }
  }

  // Now pretend to commit that block map page.
  removeReference(blockNumber, VDO_ZONE_TYPE_LOGICAL);
  // Now pretend to commit the slab journal block.
  removeReference(blockNumber, VDO_ZONE_TYPE_PHYSICAL);

  pollUntilNotReaping();
  CU_ASSERT_EQUAL(expectedHead, journal->block_map_head);
}

/**********************************************************************/
static void checkJournalStateAction(struct vdo_completion *completion)
{
  CU_ASSERT_PTR_EQUAL(VDO_ADMIN_STATE_SAVED,
                      vdo_get_admin_state_code(&journal->state));
  vdo_complete_completion(completion);
}

/**
 * Verify that the recovery journal is closed. Attempt to add an entry will
 * failed with VDO_INVALID_ADMIN_STATE as status code. Also confirm that the
 * journal is in the expected SAVED state.
 *
 * @param entry    The value for the new entry
 **/
static void verifyJournalIsClosed(EntryNumber entry)
{
  struct vdo_completion *addCompletion;
  makeWrappedVIO(entry, &addCompletion);
  launchAction(addJournalEntry, addCompletion);
  CU_ASSERT_EQUAL(VDO_INVALID_ADMIN_STATE, awaitCompletion(addCompletion));
  UDS_FREE(addCompletion);
  performSuccessfulAction(checkJournalStateAction);
}

/**
 * Exercise the journal.
 **/
static void testJournal(void)
{
  // Write one entry at a time up to the first entry of block 2.
  EntryNumber nextEntry
    = commitEntries(1, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK + 1);

  // Block the commit of block 2 and fill it.
  CompletionsWrapper block2Completions;
  EntryNumber block2Entry = nextEntry;
  nextEntry
    = launchAddWithBlockedCommit(nextEntry,
                                 RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1,
                                 &block2Completions);
  waitForAppendPoint(3, 0);
  assertLastVIOCommitted(2, 0);

  // Fill block 3 and block the commit. Verify that it is written in
  // disk but not committed.
  CompletionsWrapper block3Completions;
  EntryNumber block3Entry = nextEntry;
  nextEntry = launchAddWithBlockedCommit(nextEntry,
                                         RECOVERY_JOURNAL_ENTRIES_PER_BLOCK,
                                         &block3Completions);
  waitForAppendPoint(4, 0);
  assertLastVIOCommitted(2, 0);

  // Add one entry to block 4. Since the block is not full and block 2 is
  // still outstanding, block 4 should not be written yet.
  CompletionsWrapper block4Completions;
  EntryNumber block4Entry = nextEntry;
  blockCommit(block4Entry);
  nextEntry = launchAddEntries(block4Entry,
                               RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1,
                               &block4Completions);
  waitForAppendPoint(4, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);
  assertLastVIOCommitted(2, 0);

  // Wait 0.05 seconds to make sure a write of block 4 wasn't issued.
  usleep(50 * 1000);
  CU_ASSERT_FALSE(releaseIfLatched(pbnFromEntry(block4Entry)));

  // Let the commit of block 2 proceed and the commit point goes past block 2.
  releaseAndWaitForCompletions(block2Completions.completions, block2Entry,
                               block2Completions.count);
  freeWrappedCompletions(&block2Completions);
  verifyFullBlocks(1, 2);
  waitForAppendPoint(4, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);
  assertLastVIOCommitted(2, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);

  // Since the block 3 write is still outstanding, a partial block 4
  // write should still not have been issued.
  usleep(50 * 1000);
  CU_ASSERT_FALSE(releaseIfLatched(pbnFromEntry(block4Entry)));

  // Let the commit of block 3 proceed and the commit point goes past
  // last entry in block 3.
  releaseAndWaitForCompletions(block3Completions.completions, block3Entry,
                               block3Completions.count);
  freeWrappedCompletions(&block3Completions);

  // The write of a partial block 4 should have been triggered by the
  // completion of the block 3 write.
  waitForBlockedCommit(block4Entry);

  releaseAndWaitForCompletions(block4Completions.completions, block4Entry,
                               block4Completions.count);
  freeWrappedCompletions(&block4Completions);
  verifyFullBlocks(1, 3);
  verifyBlock(4, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);
  waitForAppendPoint(4, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);
  assertLastVIOCommitted(4, RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 2);

  // Fill the journal on disk.
  releaseAllCommits();
  block_count_t journalLength = vdo_get_recovery_journal_length(journal->size);
  nextEntry = commitEntries(nextEntry,
                            (RECOVERY_JOURNAL_ENTRIES_PER_BLOCK
                             * journalLength));

  verifyFullBlocks(1, journalLength);
  waitForAppendPoint(journalLength + 1, 0);
  assertLastVIOCommitted(journalLength,
                         RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);

  // Attempt to add another entry.
  CompletionsWrapper wrappedCompletions;
  nextEntry = launchAddEntries(nextEntry, 1, &wrappedCompletions);

  /*
   * Simulate the block map and slab journal updates for the requests
   * in block 1 assuming that each entry goes to a different, clean
   * slab journal and block map page. All the increments should be
   * done first since in the real world, they would have been done
   * before the recovery journal released its own commit lock.
   */
  verifyFullBlocks(1, 1);
  for (unsigned int i = 0; i < RECOVERY_JOURNAL_ENTRIES_PER_BLOCK; i++) {
    // Add a reference for the uncommitted slab journal block.
    CU_ASSERT_EQUAL(1, journal->slab_journal_head);
    addReference(1, VDO_ZONE_TYPE_PHYSICAL);
    // Add a reference for the uncommitted block map page.
    CU_ASSERT_EQUAL(1, journal->block_map_head);
    addReference(1, VDO_ZONE_TYPE_LOGICAL);
  }

  for (unsigned int i = 0; i < RECOVERY_JOURNAL_ENTRIES_PER_BLOCK; i++) {
    verifyFullBlocks(1, 1);
    // Remove the per-entry reference for the block map entry if this is
    // an increment.
    if (isIncrementEntry(1, i)) {
      vdo_release_journal_per_entry_lock_from_other_zone(journal, 1);
    }
    // Remove the reference for committing the slab journal block.
    CU_ASSERT_EQUAL(1, journal->slab_journal_head);
    removeReference(1, VDO_ZONE_TYPE_PHYSICAL);
    // Remove the reference for committing the block map page.
    CU_ASSERT_EQUAL(1, journal->block_map_head);
    removeReference(1, VDO_ZONE_TYPE_LOGICAL);
  }

  // Now that we've reaped the oldest journal block, our last entry
  // should get made.
  pollUntilNotReaping();
  CU_ASSERT_EQUAL(2, journal->block_map_head);
  CU_ASSERT_EQUAL(2, journal->slab_journal_head);
  waitForCompletions(wrappedCompletions.completions, 1);
  freeWrappedCompletions(&wrappedCompletions);
  verifyFullBlocks(2, journalLength);
  verifyBlock(journalLength + 1, 1);
  waitForAppendPoint(journalLength + 1, 1);
  assertLastVIOCommitted(journalLength + 1, 0);

  // Simulate the block map updates for the requests in block 3.
  simulateUpdatesForBlock(3, 2);

  // Now do the updates for the requests in blocks 2 and 4.
  simulateUpdatesForBlock(2, 4);
  simulateUpdatesForBlock(4, 5);

  // Add entries up to the first entry of block 11. The entry for 11
  // will block until we reap block 5.
  journal_entry_count_t entriesToAdd
    = (RECOVERY_JOURNAL_ENTRIES_PER_BLOCK * 4);
  nextEntry = launchAddEntries(nextEntry, entriesToAdd, &wrappedCompletions);
  waitForJournalCommit(journalLength + 4,
                       RECOVERY_JOURNAL_ENTRIES_PER_BLOCK - 1);

  // Do block map updates for block 5, to allow the close to complete.
  simulateUpdatesForBlock(5, 6);
  waitForCompletions(wrappedCompletions.completions, entriesToAdd);
  freeWrappedCompletions(&wrappedCompletions);

  // Close the journal.
  pollUntilNotReaping();
  performSuccessfulRecoveryJournalActionOnJournal(journal,
                                                  VDO_ADMIN_STATE_SAVING);
  verifyJournalIsClosed(nextEntry);
  verifyFullBlocks(6, journalLength + 4);
  verifyBlock(journalLength + 5, 1);
}

/**
 * Test that waiting VIOs are completed correctly when the recovery journal
 * goes into read-only mode, whether they are waiting on block commits or
 * stuck in a queue somewhere.
 **/
static void testReadOnlyMode(void)
{
  block_count_t  journalLength
    = vdo_get_recovery_journal_length(journal->size);
  block_count_t  entriesToAdd  = ((journalLength - 2)
                               * RECOVERY_JOURNAL_ENTRIES_PER_BLOCK);
  EntryNumber nextEntry        = commitEntries(1, entriesToAdd);
  injectWriteError             = true;
  blockAllCommits();

  CompletionsWrapper wrappedCompletions;
  entriesToAdd = (2 * RECOVERY_JOURNAL_ENTRIES_PER_BLOCK) + 1;

  EntryNumber block5Entry = nextEntry;
  EntryNumber block6Entry = block5Entry + RECOVERY_JOURNAL_ENTRIES_PER_BLOCK;
  nextEntry = launchAddEntries(nextEntry, entriesToAdd, &wrappedCompletions);
  waitForBlockedCommit(block5Entry);
  waitForBlockedCommit(block6Entry);
  CU_ASSERT_TRUE(has_waiters(&journal->increment_waiters));

  releaseAllCommits();

  for (unsigned int i = 0; i < wrappedCompletions.count; i++) {
    CU_ASSERT_EQUAL(awaitCompletion(wrappedCompletions.completions[i]),
                    VDO_READ_ONLY);
  }
  freeWrappedCompletions(&wrappedCompletions);

  // Tell the journal to close.
  CU_ASSERT_EQUAL(performRecoveryJournalAction(journal,
                                               VDO_ADMIN_STATE_SAVING),
                  VDO_READ_ONLY);
  verifyJournalIsClosed(nextEntry);
}

/**
 * An IncrementSelector which always increments.
 **/
static enum journal_operation alwaysIncrement(void)
{
  return VDO_JOURNAL_DATA_INCREMENT;
}

/**
 * An IncrementSelector which always decrements.
 **/
static enum journal_operation alwaysDecrement(void)
{
  return VDO_JOURNAL_DATA_DECREMENT;
}

/**
 * Wait for the append point to reach that of a given entry.
 *
 * @param entry  The entry whose append point should be waited on
 **/
static void waitForAppendPointForEntry(EntryNumber entry)
{
  sequence_number_t sequenceNumber = sequenceNumberFromEntry(entry);
  journal_entry_count_t entryCount
    = entry - ((sequenceNumber - 1) * journal->entries_per_block);
  waitForAppendPoint(sequenceNumber, entryCount);
}

/**
 * VDOAction to release all recovery journal locks on a given journal block.
 *
 * @param completion  The action completion
 **/
static void unlockJournalBlock(struct vdo_completion *completion)
{
  // This method depends on the single thread config.
  if (!vdo_is_lock_locked(journal->lock_counter,
                          recoverySequenceNumber % journal->size,
                          VDO_ZONE_TYPE_LOGICAL)) {
    vdo_finish_completion(completion, VDO_SUCCESS);
    return;
  }

  /*
   * If the block is locked, acquire an extra logical zone reference which we
   * can release after we've released all the journal locks in order to cause
   * the journal to reap.
   */
  vdo_acquire_recovery_journal_block_reference(journal, recoverySequenceNumber,
                                               VDO_ZONE_TYPE_LOGICAL, 0);

  while (vdo_is_lock_locked(journal->lock_counter,
                            recoverySequenceNumber % journal->size,
                            VDO_ZONE_TYPE_PHYSICAL)) {
    vdo_release_journal_per_entry_lock_from_other_zone(journal,
                                                       recoverySequenceNumber);
  }

  vdo_release_recovery_journal_block_reference(journal, recoverySequenceNumber,
                                               VDO_ZONE_TYPE_LOGICAL, 0);

  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Test that decrements are given precedence over increments and that
 * increment entries are not made if there is not journal space for the
 * ensuing decrement entry.
 **/
static void testIncrementDecrementPolicy(void)
{
  // Make all entries be increments.
  shouldBeIncrement = alwaysIncrement;

  // Write 1 more than the number of increments that will fit.
  CompletionsWrapper incrementCompletions;
  EntryNumber lastEntry
    = launchAddEntries(0, (journal->available_space / 2) + 1,
                       &incrementCompletions);
  waitForAppendPointForEntry(lastEntry);
  waitForCompletions(incrementCompletions.completions,
                     incrementCompletions.count - 1);
  CU_ASSERT_TRUE(has_waiters(&journal->increment_waiters));

  // Write a decrement which should fit but the increment should still be
  // blocked.
  shouldBeIncrement = alwaysDecrement;
  addOneEntry(lastEntry);
  CU_ASSERT_TRUE(has_waiters(&journal->increment_waiters));

  // Reap the head of the journal to free up available space which should
  // allow the last increment to make its entry.
  recoverySequenceNumber = 1;
  performSuccessfulAction(unlockJournalBlock);

  struct vdo_completion *completion
    = incrementCompletions.completions[incrementCompletions.count - 1];
  VDO_ASSERT_SUCCESS(awaitCompletion(completion));
  freeWrappedCompletions(&incrementCompletions);

  // Verify that the decrement entry precedes the last increment entry.
  sequence_number_t lastBlock = sequenceNumberFromEntry(lastEntry);
  struct packed_journal_header *header
    = getJournalBlockFromLayer(lastBlock);
  struct packed_journal_sector *sector
    = vdo_get_journal_block_sector(header, 1);

  EntryNumber entryNumber = sector->entry_count - 2;
  struct recovery_journal_entry entry
    = vdo_unpack_recovery_journal_entry(&sector->entries[entryNumber]);
  CU_ASSERT_EQUAL(entry.slot.pbn, lastEntry);
  CU_ASSERT_FALSE(vdo_is_journal_increment_operation(entry.operation));

  entryNumber = sector->entry_count - 1;
  entry
    = vdo_unpack_recovery_journal_entry(&sector->entries[entryNumber]);
  CU_ASSERT_EQUAL(entry.slot.pbn, lastEntry - 1);
  CU_ASSERT_TRUE(vdo_is_journal_increment_operation(entry.operation));

  UDS_FREE(header);
}

/**********************************************************************/
static CU_TestInfo recoveryJournalTests[] = {
  { "encode/decode",             testEncodeDecode             },
  { "entry pack/unpack",         testEntryPacking             },
  { "block header pack/unpack",  testBlockHeaderPacking       },
  { "exercise journal",          testJournal                  },
  { "read-only mode",            testReadOnlyMode             },
  { "decrement priority",        testIncrementDecrementPolicy },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo recoveryJournalSuite = {
  .name                     = "Recovery tests (RecoveryJournal_t1)",
  .initializerWithArguments = NULL,
  .initializer              = createLayerAndJournal,
  .cleaner                  = freeLayerAndJournal,
  .tests                    = recoveryJournalTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &recoveryJournalSuite;
}
