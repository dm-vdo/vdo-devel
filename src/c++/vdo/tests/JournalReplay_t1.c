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
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "ioRequest.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static struct slab_depot       *depot;
static struct recovery_journal *recoveryJournal;

/**
 * Get a slab journal from a specific slab.
 *
 * @param  slabNumber  the slab number of the slab journal
 **/
static struct slab_journal *getVDOSlabJournal(slab_count_t slabNumber)
{
  return depot->slabs[slabNumber]->journal;
}

/**
 * Test-specific initialization.
 **/
static void initialize(void)
{
  const TestParameters parameters = {
    .mappableBlocks    = 256,
    .journalBlocks     = 16,
    .slabJournalBlocks = 8,
  };
  initializeVDOTest(&parameters);

  depot           = vdo->depot;
  recoveryJournal = vdo->recovery_journal;

  // Fill the physical space.
  fillPhysicalSpace(1, 1);

  // Flush block map and slab journals to release all recovery journal locks.
  performSuccessfulBlockMapAction(VDO_ADMIN_STATE_RECOVERING);
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RECOVERING);

  // vdo_slab journals are flushed.
  for (slab_count_t slab = 1; slab < depot->slab_count; slab++) {
    struct slab_journal *slabJournal = getVDOSlabJournal(slab);
    CU_ASSERT_EQUAL(slabJournal->last_summarized, 2);
  }
}

/**
 * Simulate a VDO crash and restart it as dirty.
 */
static void crashAndRebuildVDO(void)
{
  crashVDO();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
  depot           = vdo->depot;
  recoveryJournal = vdo->recovery_journal;
}

/**
 * Test that recovery journal and slab journal entries for a decRef is not
 * replayed at all if the corresponding refCount update, slab journal entry and
 * the recovery journal entry are all committed to disk, even if the slab is
 * marked dirty in the slab summary.
 **/
static void testNoReplay(void)
{
  // A trim creates an incRef and a decRef in the recovery journal, a decRef at
  // a slab journal, and a refCount update.
  physical_block_number_t trimmedPBN = lookupLBN(17).pbn;
  VDO_ASSERT_SUCCESS(performTrim(17, 1));

  struct vdo_slab *dirtySlab       = vdo_get_slab(depot, trimmedPBN);
  slab_count_t     dirtySlabNumber = dirtySlab->slab_number;
  CU_ASSERT_EQUAL(vdo_count_unreferenced_blocks(dirtySlab->reference_counts,
                                                dirtySlab->start,
                                                dirtySlab->end), 1);

  // Force all slab journal tail blocks to be written out.
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RECOVERING);

  // Write out the RefCounts for the slab.
  performSuccessfulSlabAction(dirtySlab, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);

  // Mark the slab as dirty in slab summary to force scrubbing on next restart.
  // Note that the free block count should be recalculated during scrubbing.
  struct slab_journal *slabJournal = getVDOSlabJournal(dirtySlab->slab_number);
  tail_block_offset_t tailBlockOffset
    = vdo_get_slab_journal_block_offset(slabJournal,
                                        slabJournal->last_summarized);
  struct slab_summary_zone *summary = depot->slab_summary->zones[0];
  bool loadRefCounts
    = vdo_must_load_ref_counts(summary, dirtySlab->slab_number);
  performSlabSummaryUpdate(summary, dirtySlab->slab_number, tailBlockOffset,
                           loadRefCounts, false, 1000);
  CU_ASSERT_FALSE(vdo_get_summarized_cleanliness(summary,
                                                 dirtySlab->slab_number));

  crashAndRebuildVDO();

  dirtySlab = depot->slabs[dirtySlabNumber];
  block_count_t unreferenced
    = vdo_count_unreferenced_blocks(dirtySlab->reference_counts,
                                    dirtySlab->start,
                                    dirtySlab->end);
  CU_ASSERT_EQUAL(unreferenced, 1);
}

/**********************************************************************/
static CU_TestInfo vdoTests[] = {
  { "replay none", testNoReplay },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name  = "journal replay (JournalReplay_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initialize,
  .cleaner                  = tearDownVDOTest,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
