/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "read-only-notifier.h"
#include "slab.h"
#include "slab-summary.h"
#include "vdo-layout.h"

#include "asyncLayer.h"
#include "latchedCloseUtils.h"
#include "mutexUtils.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

const block_count_t BLOCK_COUNT              = 400;
const block_count_t MAX_FREE_BLOCKS_PER_SLAB = 1 << 22;

enum fakeErrorCodes {
  WRITE_ERROR  = -1,
};

static struct fixed_layout       *layout;
static struct partition          *slabSummaryPartition;
static struct partition          *slabPartition;
static struct read_only_notifier *readOnlyNotifier;
static struct thread_config      *threadConfig;
static struct slab_summary       *summary;
static struct slab_summary_zone  *summaryZone;
static struct slab_status        *statuses;

/**********************************************************************/
static block_count_t getDefaultFreeBlocks(size_t id)
{
  return (id << 10);
}

/**********************************************************************/
static size_t getDefaultFreeBlockHint(size_t id)
{
  block_count_t freeBlocks   = getDefaultFreeBlocks(id);
  size_t        expectedHint = freeBlocks >> 17;
  if ((freeBlocks != 0) && (expectedHint == 0)) {
    expectedHint = 1;
  }
  return expectedHint;
}

/**********************************************************************/
static block_count_t getDefaultTailBlockOffset(size_t id)
{
  return (id % DEFAULT_VDO_SLAB_JOURNAL_SIZE);
}

/**********************************************************************/
static bool getDefaultCleanliness(size_t id)
{
  return ((id & 0x40) > 0);
}

/**
 * Initialize a client to use a default data pattern, based on its ID.
 *
 * @param client    The client to initialize
 * @param id        The indentifier of the client
 **/
static void useDefaultPattern(SlabSummaryClient *client, size_t id)
{
  initializeSlabSummaryClient(client, id, summaryZone);
  client->freeBlocks      = getDefaultFreeBlocks(id);
  client->tailBlockOffset = getDefaultTailBlockOffset(id);
  client->isClean         = getDefaultCleanliness(id);
}

/**
 * Check whether this is a slab summary write.
 *
 * Implements BlockCondition
 **/
static bool isSlabSummaryWrite(struct vdo_completion *completion,
                               void *context __attribute__((unused)))
{
  return (vioTypeIs(completion, VIO_TYPE_SLAB_SUMMARY)
          && isMetadataWrite(completion));
}

/**
 * Inject a write error in a slab summary write.
 *
 * Implements BIOSubmitHook.
 **/
static bool injectWriteError(struct bio *bio)
{
  struct vio *vio = bio->bi_private;

  if ((vio != NULL) && isSlabSummaryWrite(vio_as_completion(vio), NULL)) {
    vdo_set_completion_result(vio_as_completion(vio), WRITE_ERROR);
    clearBIOSubmitHook();
  }

  return true;
}

/**
 * Release a latched vio, giving it the specified status code.
 *
 * @param vio         The latched vio
 * @param statusCode  The status to give the vio
 **/
static void releaseLatchedVIO(struct vio *vio, int statusCode)
{
  CU_ASSERT_TRUE(vio != NULL);
  vdo_set_completion_result(vio_as_completion(vio), statusCode);
  vio->bio->bi_end_io(vio->bio);
}

/**
 * Set up a slab_summary and layers for test purposes.
 **/
static void initializeSlabSummary(void)
{
  TestParameters testParameters = {
    .mappableBlocks = BLOCK_COUNT,
    .noIndexRegion  = true,
  };
  initializeBasicTest(&testParameters);

  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout(BLOCK_COUNT, 0, &layout));

  int result = vdo_make_fixed_layout_partition(layout,
                                               VDO_SLAB_SUMMARY_PARTITION,
                                               VDO_SLAB_SUMMARY_BLOCKS,
                                               VDO_PARTITION_FROM_END,
                                               0);
  VDO_ASSERT_SUCCESS(result);

  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout,
						    VDO_SLAB_SUMMARY_PARTITION,
						    &slabSummaryPartition));
  threadConfig = makeOneThreadConfig();
  VDO_ASSERT_SUCCESS(vdo_make_read_only_notifier(false,
                                                 threadConfig,
                                                 vdo,
                                                 &readOnlyNotifier));
  VDO_ASSERT_SUCCESS(vdo_make_slab_summary(vdo,
                                           slabSummaryPartition,
                                           threadConfig,
                                           23,
                                           MAX_FREE_BLOCKS_PER_SLAB,
                                           readOnlyNotifier,
                                           &summary));
  summaryZone = vdo_get_slab_summary_for_zone(summary, 0);

  result
    = vdo_make_fixed_layout_partition(layout, VDO_BLOCK_ALLOCATOR_PARTITION,
                                      VDO_ALL_FREE_BLOCKS,
                                      VDO_PARTITION_FROM_BEGINNING, 0);
  VDO_ASSERT_SUCCESS(result);

  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout,
						    VDO_BLOCK_ALLOCATOR_PARTITION,
						    &slabPartition));

  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(MAX_VDO_SLABS, struct slab_status, __func__,
                                  &statuses));
}

/**
 * Tear down a slab_summary and its associated variables and layers.
 **/
static void tearDownSlabSummary(void)
{
  UDS_FREE(statuses);
  int result = closeSlabSummary(summary);
  CU_ASSERT_TRUE((result == VDO_SUCCESS) || (result == VDO_READ_ONLY));
  vdo_free_slab_summary(UDS_FORGET(summary));
  vdo_free_read_only_notifier(UDS_FORGET(readOnlyNotifier));
  vdo_free_thread_config(UDS_FORGET(threadConfig));
  vdo_free_fixed_layout(UDS_FORGET(layout));
  tearDownVDOTest();
}

/**
 * Test that slab_summary_entry always maps the bit fields to the
 * correct bits of the on-disk encoding.
 **/
static void testEntryEncoding(void)
{
  union {
    struct slab_summary_entry fields;
    u8 raw[2];
  } entry = {
    .raw = { 0, 0 }
  };
  STATIC_ASSERT(sizeof(entry) == 2);

  // tailBlockOffset is in the first byte.
  entry.fields.tail_block_offset = 0xA5;
  CU_ASSERT_EQUAL(0xA5, entry.raw[0]);

  // Turn on bits field-by-field to ensure they're mapped correctly.
  CU_ASSERT_EQUAL(0x00, entry.raw[1]);
  // isDirty is bit 15, the high bit of the second byte.
  entry.fields.is_dirty = true;
  CU_ASSERT_EQUAL(0x80, entry.raw[1]);
  // loadRefCounts is bit 14, the second-highest bit of the second byte.
  entry.fields.load_ref_counts = true;
  CU_ASSERT_EQUAL(0xC0, entry.raw[1]);
  // fullnessHint occupies the remaining six bits of the second byte.
  entry.fields.fullness_hint = (1 << 6) - 1;
  CU_ASSERT_EQUAL(0xFF, entry.raw[1]);
}

/**
 * An action to load the slab summary.
 *
 * @param completion  The completion for the action
 **/
static void loadSlabSummaryAction(struct vdo_completion *completion)
{
  vdo_load_slab_summary(summary, VDO_ADMIN_STATE_LOADING, 1, completion);
}

/**
 * Load a slab_summary from the given partition.
 *
 * @param partition The partition to load from
 **/
static void loadSlabSummaryFromPartition(struct partition *partition)
{
  VDO_ASSERT_SUCCESS(vdo_make_slab_summary(vdo,
                                           partition,
                                           threadConfig,
                                           23,
                                           MAX_FREE_BLOCKS_PER_SLAB,
                                           readOnlyNotifier,
                                           &summary));
  summaryZone = vdo_get_slab_summary_for_zone(summary, 0);
  performSuccessfulAction(loadSlabSummaryAction);
}

/**
 * Save, free, and reload a slab_summary.
 **/
static void cleanRestartSlabSummary(void)
{
  VDO_ASSERT_SUCCESS(drainSlabSummary(summary));
  vdo_free_slab_summary(UDS_FORGET(summary));
  loadSlabSummaryFromPartition(slabSummaryPartition);
}

/**
 * Writes the default data pattern to the slab_summary, parallelized.
 **/
static void writeDefaultDataPattern(void)
{
  // Make MAX_VDO_SLABS slab summary updates.
  SlabSummaryClient *clients;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(MAX_VDO_SLABS, SlabSummaryClient, __func__,
                                  &clients));
  for (size_t id = 0; id < MAX_VDO_SLABS; id++) {
    useDefaultPattern(&clients[id], id);
  }

  // Launch all MAX_VDO_SLABS simultaneously.
  for (size_t id = 0; id < MAX_VDO_SLABS; id++) {
    launchUpdateSlabSummaryEntry(&clients[id]);
  }

  // Await all MAX_VDO_SLABS being finished.
  for (size_t id = 0; id < MAX_VDO_SLABS; id++) {
    VDO_ASSERT_SUCCESS(awaitCompletion(&clients[id].completion));
  }
  UDS_FREE(clients);
}

/**
 * Get a slab summary entry for a slab, given a test completion.
 *
 * @param completion    The completion to fill
 **/
static void doGetSlabSummaryEntry(struct vdo_completion *completion)
{
  SlabSummaryClient *client = completionAsSlabSummaryClient(completion);
  client->tailBlockOffset
    = vdo_get_summarized_tail_block_offset(summaryZone, client->slabOffset);
  struct slab_summary_entry *entry = &summaryZone->entries[client->slabOffset];
  client->freeBlockHint = entry->fullness_hint;
  client->isClean = !entry->is_dirty;
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Assert that the stored slab_summary_entry for a given slab is as expected.
 *
 * @param id                      The ID of the client
 * @param expectedTailBlockOffset The expected tail block offset
 * @param expectedFreeBlocks      The expected approximate free block number
 * @param expectedCleanliness     The expected cleanliness state
 **/
static void assertSlabSummaryEntry(size_t        id,
                                   size_t        expectedTailBlockOffset,
                                   block_count_t expectedFreeBlockHint,
                                   bool          expectedCleanliness)
{
  SlabSummaryClient client;
  initializeSlabSummaryClient(&client, id, summaryZone);
  VDO_ASSERT_SUCCESS(performAction(doGetSlabSummaryEntry, &client.completion));
  CU_ASSERT_EQUAL(client.freeBlockHint,   expectedFreeBlockHint);
  CU_ASSERT_EQUAL(client.tailBlockOffset, expectedTailBlockOffset);
  CU_ASSERT_EQUAL(client.isClean,         expectedCleanliness);
}

/**
 * Get all slab statuses, given a test completion.
 *
 * @param  completion   The completion to fill
 **/
static void doGetSummarizedSlabStatuses(struct vdo_completion *completion)
{
  SlabSummaryClient *client = completionAsSlabSummaryClient(completion);
  vdo_get_summarized_slab_statuses(summaryZone, MAX_VDO_SLABS,
                                   client->statuses);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**
 * Fetch all slab statuses using vdo_get_summarized_slab_statuses()).
 *
 * @param [in,out] statuses     An array to hold the fetched slab statuses
 **/
static void fetchSlabStatuses(struct slab_status *statuses)
{
  SlabSummaryClient statusClient;
  initializeSlabSummaryClient(&statusClient, 0, summaryZone);
  statusClient.statuses = statuses;
  VDO_ASSERT_SUCCESS(performAction(doGetSummarizedSlabStatuses,
                                   &statusClient.completion));
}

/**
 * Assert that the contents of the slab_summary, starting at slab index
 * startIndex, are filled with the default data pattern written by
 * writeDefaultDataPattern().
 *
 * @param startIndex    The first slab number to verify
 **/
static void verifyDefaultDataPattern(size_t startIndex)
{
  fetchSlabStatuses(statuses);
  for (slab_count_t slabNumber = startIndex; slabNumber < MAX_VDO_SLABS;
       slabNumber++) {
    assertSlabSummaryEntry(slabNumber, getDefaultTailBlockOffset(slabNumber),
                           getDefaultFreeBlockHint(slabNumber),
                           getDefaultCleanliness(slabNumber));
    CU_ASSERT_EQUAL(statuses[slabNumber].is_clean,
                    getDefaultCleanliness(slabNumber));
    CU_ASSERT_EQUAL(statuses[slabNumber].emptiness,
                    getDefaultFreeBlockHint(slabNumber));
  }
}

/**
 * Test serially writing some data in the slab_summary, then cleanly
 * restarting and verifying that all data remains.
 **/
static void testSaveAndRestore(void)
{
  // Make a slab summary and update it with a bunch of serial writes.
  SlabSummaryClient client;
  // MAX_VDO_SLABS serial writes turns out to take a long time.
  size_t start = (MAX_VDO_SLABS > 200) ? (MAX_VDO_SLABS - 200) : 0;
  for (size_t id = start; id < MAX_VDO_SLABS; id++) {
    useDefaultPattern(&client, id);
    VDO_ASSERT_SUCCESS(performAction(doUpdateSlabSummaryEntry,
                                     &client.completion));
    vdo_reset_completion(&client.completion);
  }

  cleanRestartSlabSummary();
  verifyDefaultDataPattern(8000);
}

/**
 * Test all slab summary entries being updated at once.
 **/
static void testBasicWrite(void)
{
  writeDefaultDataPattern();
  cleanRestartSlabSummary();
  verifyDefaultDataPattern(0);
}

/**
 * Action to assert that the VDO is in read-only mode.
 **/
static void assertReadOnlyAction(struct vdo_completion *completion)
{
  CU_ASSERT(vdo_is_read_only(readOnlyNotifier));
  vdo_complete_completion(completion);
}

/**
 * Test updating the slab_summary, but encountering a write error; verify that
 * later updates also fail.
 **/
static void testBasicWriteError(void)
{
  writeDefaultDataPattern();

  // Update with new values different than the defaults.
  SlabSummaryClient client;
  useDefaultPattern(&client, 0);
  client.freeBlocks = (1 << 23) - 1;
  client.isClean    = true;

  // Prepare to inject a write error
  setBIOSubmitHook(injectWriteError);
  launchUpdateSlabSummaryEntry(&client);
  CU_ASSERT_EQUAL(awaitCompletion(&client.completion), VDO_READ_ONLY);
  performSuccessfulAction(assertReadOnlyAction);

  // Check that future updates don't work either.
  useDefaultPattern(&client, 0);
  client.freeBlocks = (1 << 23) - 1;
  client.isClean    = true;
  CU_ASSERT_EQUAL(performAction(doUpdateSlabSummaryEntry, &client.completion),
                  VDO_READ_ONLY);
}

/**
 * Test failing a write while updates to the block are pending
 * does not cause a close to hang.
 **/
static void testPendingUpdatesError(void)
{
  writeDefaultDataPattern();

  // Make two updates on the same block.
  SlabSummaryClient firstBlockClients[2];
  initializeSlabSummaryClient(&firstBlockClients[0], 0, summaryZone);
  initializeSlabSummaryClient(&firstBlockClients[1], 1, summaryZone);
  firstBlockClients[0].freeBlocks = (1 << 23) - 1;
  firstBlockClients[0].isClean    = true;
  firstBlockClients[1].freeBlocks = (1 << 23) - 1;
  firstBlockClients[1].isClean    = true;

  // Launch the first update and latch its commit.
  setBlockBIO(isSlabSummaryWrite, true);
  launchUpdateSlabSummaryEntry(&firstBlockClients[0]);
  struct vio *firstBlockWrite = getBlockedVIO();

  // Launch the second update and wait for it to be queued, then release
  // the blocked slab summary write.
  enqueueUpdateSlabSummaryEntry(&firstBlockClients[1]);
  releaseLatchedVIO(firstBlockWrite, WRITE_ERROR);

  // Ensure that all waiters on that block return VDO_READ_ONLY.
  CU_ASSERT_EQUAL(awaitCompletion(&firstBlockClients[0].completion),
                  VDO_READ_ONLY);
  CU_ASSERT_EQUAL(awaitCompletion(&firstBlockClients[1].completion),
                  VDO_READ_ONLY);

  // Issue a close, which should finish with VDO_READ_ONLY.
  CU_ASSERT_EQUAL(closeSlabSummary(summary), VDO_READ_ONLY);
}

/**
 * Launch a summary close.
 *
 * <p>Implements CloseLauncher.
 **/
static void launchSummaryClose(void *context, struct vdo_completion *parent)
{
  vdo_drain_slab_summary_zone((struct slab_summary_zone *) context,
                              VDO_ADMIN_STATE_SAVING,
                              parent);
}

/**
 * Check whether the summary is closed. Implements ClosednessVerifier.
 **/
static bool checkSummaryClosed(void *context)
{
  struct slab_summary_zone *summaryZone = context;
  return vdo_is_state_quiescent(&summaryZone->state);
}

/**
 * Release the two blocked writes. Implements BlockedIOReleaser.
 **/
static void releaseBlockedSummaryWrites(void *context)
{
  struct vio **blockedWrites = context;
  // Release both blocks.
  releaseLatchedVIO(blockedWrites[0], VDO_SUCCESS);
  releaseLatchedVIO(blockedWrites[1], VDO_SUCCESS);
}

/**
 * An action to put the read_only_notifier in read-only mode and wait for its
 * notifications to finish.
 **/
static void readOnlyModeAction(struct vdo_completion *completion)
{
  vdo_enter_read_only_mode(readOnlyNotifier, VDO_READ_ONLY);
  vdo_wait_until_not_entering_read_only_mode(readOnlyNotifier, completion);
}

/**
 * Test updating the slab_summary on multiple blocks at once, and having an
 * external cause make the system go into read-only mode.
 **/
static void testReadOnlyDuringWrite(void)
{
  writeDefaultDataPattern();

  // Make two updates on the same block.
  SlabSummaryClient firstBlockClients[2];
  initializeSlabSummaryClient(&firstBlockClients[0], 0, summaryZone);
  initializeSlabSummaryClient(&firstBlockClients[1], 1, summaryZone);
  firstBlockClients[0].freeBlocks = (1 << 23) - 1;
  firstBlockClients[0].isClean    = true;
  firstBlockClients[1].freeBlocks = (1 << 23) - 1;
  firstBlockClients[1].isClean    = true;

  // Launch the first update and latch its commit.
  setBlockVIOCompletionEnqueueHook(isSlabSummaryWrite, true);
  launchUpdateSlabSummaryEntry(&firstBlockClients[0]);
  struct vio *blockedWrites[2];
  blockedWrites[0] = getBlockedVIO();

  // Launch the second update and wait for it to be queued.
  enqueueUpdateSlabSummaryEntry(&firstBlockClients[1]);

  // Launch and latch an update to a different block, then update the block
  // again. Skipping MAX_VDO_SLABS / 2 slabs will land on a different block.
  SlabSummaryClient secondBlockClients[2];
  initializeSlabSummaryClient(&secondBlockClients[0], MAX_VDO_SLABS / 2,
                              summaryZone);
  initializeSlabSummaryClient(&secondBlockClients[1], (MAX_VDO_SLABS / 2) + 1,
                              summaryZone);
  secondBlockClients[0].freeBlocks = 18 << 17;
  secondBlockClients[0].isClean    = true;
  secondBlockClients[1].freeBlocks = 19 << 17;
  secondBlockClients[1].isClean    = true;
  setBlockVIOCompletionEnqueueHook(isSlabSummaryWrite, true);
  launchUpdateSlabSummaryEntry(&secondBlockClients[0]);
  blockedWrites[1] = getBlockedVIO();

  enqueueUpdateSlabSummaryEntry(&secondBlockClients[1]);

  // Issue a save, which should wait for both blocks to finish writing.
  performSuccessfulAction(readOnlyModeAction);
  CloseInfo closeInfo = (CloseInfo) {
    .launcher       = launchSummaryClose,
    .checker        = checkSummaryClosed,
    .closeContext   = summaryZone,
    .releaser       = releaseBlockedSummaryWrites,
    .releaseContext = blockedWrites,
    .threadID       = vdo_get_physical_zone_thread(threadConfig,
                                                   summaryZone->zone_number),
  };
  runLatchedClose(closeInfo, VDO_READ_ONLY);

  // Ensure that all waiters returned VDO_READ_ONLY.
  CU_ASSERT_EQUAL(awaitCompletion(&firstBlockClients[0].completion),
                  VDO_READ_ONLY);
  CU_ASSERT_EQUAL(awaitCompletion(&firstBlockClients[1].completion),
                  VDO_READ_ONLY);
  CU_ASSERT_EQUAL(awaitCompletion(&secondBlockClients[0].completion),
                  VDO_READ_ONLY);
  CU_ASSERT_EQUAL(awaitCompletion(&secondBlockClients[1].completion),
                  VDO_READ_ONLY);

  // Another save should immediately return VDO_SUCCESS without launching any
  // IO.
  setBlockVIOCompletionEnqueueHook(isSlabSummaryWrite, true);
  VDO_ASSERT_SUCCESS(drainSlabSummary(summary));
  assertNoBlockedVIOs();
}

/**
 * Test that simultaneous updates to a block both succeed.
 **/
static void testBlockSimultaneousUpdate(void)
{
  writeDefaultDataPattern();

  // Make two updates on the same block with different values.
  SlabSummaryClient clients[2];
  initializeSlabSummaryClient(&clients[0], 0, summaryZone);
  initializeSlabSummaryClient(&clients[1], 1, summaryZone);
  clients[0].freeBlocks      = (1 << 23) - 1;
  clients[0].tailBlockOffset = 35;
  clients[0].isClean         = true;
  clients[1].freeBlocks      = (1 << 23) - 1;
  clients[1].tailBlockOffset = 29;
  clients[1].isClean         = true;

  // Launch the first and latch it.
  setBlockVIOCompletionEnqueueHook(isSlabSummaryWrite, true);
  launchUpdateSlabSummaryEntry(&clients[0]);
  struct vio *latched = getBlockedVIO();

  // Launch the second and wait for it to be queued.
  enqueueUpdateSlabSummaryEntry(&clients[1]);

  // Release the first update.
  releaseLatchedVIO(latched, VDO_SUCCESS);

  // Wait for both to come back.
  VDO_ASSERT_SUCCESS(awaitCompletion(&clients[0].completion));
  VDO_ASSERT_SUCCESS(awaitCompletion(&clients[1].completion));

  // Verify 0 and 1 are now updated.
  assertSlabSummaryEntry(0, 35, 0x3f, true);
  assertSlabSummaryEntry(1, 29, 0x3f, true);

  verifyDefaultDataPattern(2);
}

/**
 * Test that multiple updates to the same slab summary all succeed and are
 * ordered correctly.
 **/
static void testSlabSimultaneousUpdate(void)
{
  writeDefaultDataPattern();

  // Make three updates to the same location to different values.
  SlabSummaryClient clients[3];
  initializeSlabSummaryClient(&clients[0], 0, summaryZone);
  initializeSlabSummaryClient(&clients[1], 0, summaryZone);
  initializeSlabSummaryClient(&clients[2], 0, summaryZone);
  clients[0].freeBlocks      = (1 << 23) - 1;
  clients[0].tailBlockOffset = 228;
  clients[0].isClean         = true;
  clients[1].freeBlocks      = (1 << 22) - 1;
  clients[1].tailBlockOffset = 28;
  clients[1].isClean         = false;
  clients[2].freeBlocks      = (1 << 21) - 1;
  clients[2].tailBlockOffset = 38;
  clients[2].isClean         = false;

  // Launch the first and latch it.
  setBlockVIOCompletionEnqueueHook(isSlabSummaryWrite, true);
  launchUpdateSlabSummaryEntry(&clients[0]);
  struct vio *latched = getBlockedVIO();

  // Launch the second and wait for it to be queued.
  enqueueUpdateSlabSummaryEntry(&clients[1]);

  // Launch the third and wait for it to be queued.
  enqueueUpdateSlabSummaryEntry(&clients[2]);

  // Release the first update.
  releaseLatchedVIO(latched, VDO_SUCCESS);

  // Wait for all to come back.
  VDO_ASSERT_SUCCESS(awaitCompletion(&clients[0].completion));
  VDO_ASSERT_SUCCESS(awaitCompletion(&clients[1].completion));
  VDO_ASSERT_SUCCESS(awaitCompletion(&clients[2].completion));

  // Verify that, after all updates have been completed, the last update
  // is the remaining one.
  assertSlabSummaryEntry(0, 38, (0xf), false);

  // Verify that the rest of the data is still correct.
  verifyDefaultDataPattern(1);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test entry encoding"                     , testEntryEncoding           },
  { "basic test of serial writes save/restore", testSaveAndRestore          },
  { "simultaneous writes"                     , testBasicWrite              },
  { "test of a write error during update"     , testBasicWriteError         },
  { "write error with uncommitted updates"    , testPendingUpdatesError     },
  { "read-only mode with uncommitted updates" , testReadOnlyDuringWrite     },
  { "simultaneous updates on same block"      , testBlockSimultaneousUpdate },
  { "simultaneous updates of same slab"       , testSlabSimultaneousUpdate  },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "slab_summary tests (SlabSummary_t1)",
  .initializer = initializeSlabSummary,
  .cleaner     = tearDownSlabSummary,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
