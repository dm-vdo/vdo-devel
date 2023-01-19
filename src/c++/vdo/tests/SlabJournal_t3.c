/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "block-allocator.h"
#include "slab.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  struct vdo_completion completion;
  struct data_vio       dataVIO;
} DataVIOWrapper;

static const TestParameters TEST_PARAMETERS = {
  .slabCount = 4,
};

static struct slab_journal *journal;

/**
 * Setup physical and asynchronous layer, then create 4 slab journals to
 * use the asynchronous layer.
 **/
static void slabJournalTestInitialization(void)
{
  initializeVDOTest(&TEST_PARAMETERS);
}

/**
 * Initialize a VIO wrapped in a wrapping completion.
 *
 * @param wrapper  The wrapper to initialize
 **/
static void initializeWrapper(DataVIOWrapper *wrapper)
{
  struct data_vio *dataVIO = &wrapper->dataVIO;
  vdo_initialize_completion(&wrapper->completion, vdo, VDO_TEST_COMPLETION);
  vdo_initialize_completion(&dataVIO->vio.completion, vdo, VIO_COMPLETION);
  dataVIO->vio.type         = VIO_TYPE_DATA;
  dataVIO->mapped.state     = VDO_MAPPING_STATE_UNCOMPRESSED;
  dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
}

/**
 * Reset the VIO wrapper and the VIO it contains.
 *
 * @param wrapper     The wrapper to reset
 * @param slabNumber  The slab in which the VIO should make an increment entry
 **/
static void resetWrapper(DataVIOWrapper *wrapper, slab_count_t slabNumber)
{
  struct data_vio *dataVIO = &wrapper->dataVIO;
  vdo_reset_completion(&wrapper->completion);
  vdo_reset_completion(&dataVIO->vio.completion);
  dataVIO->vio.completion.callback = vdo_finish_completion_parent_callback;
  dataVIO->vio.completion.parent   = &wrapper->completion;
  dataVIO->logical.lbn             = (logical_block_number_t) slabNumber;

  struct vdo_slab *slab = vdo->depot->slabs[slabNumber];
  journal               = slab->journal;

  dataVIO->new_mapped.pbn = slab->start + 1;
  dataVIO->operation = (struct reference_operation) {
    .type = VDO_JOURNAL_DATA_INCREMENT,
    .pbn  = slab->start + 1,
  };
  dataVIO->recovery_journal_point = (struct journal_point) {
    .sequence_number = slabNumber + 1,
    .entry_count     = slabNumber,
  };
}

/**
 * Construct a VIO wrapped in a completion.
 *
 * @param slabNumber     The slab in which the VIO should make an increment
 *                       entry
 * @param completionPtr  A pointer to hold the wrapper as a completion
 **/
static void makeWrappedVIO(slab_count_t            slabNumber,
                           struct vdo_completion **completionPtr)
{
  DataVIOWrapper *wrapper;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, DataVIOWrapper, __func__, &wrapper));
  initializeWrapper(wrapper);
  resetWrapper(wrapper, slabNumber);
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
 * The action to add an entry to the journal.
 *
 * @param completion A wrapper containing the VIO for which to add an entry
 **/
static void addSlabJournalEntryAction(struct vdo_completion *completion)
{
  vdo_add_slab_journal_entry(journal, dataVIOFromWrapper(completion));
}

/**
 * Construct a wrapped VIO and perform an action to add an entry for it in the
 * journal.
 *
 * @param slabNumber  The number of the journal entry
 **/
static void performAddEntry(slab_count_t slabNumber)
{
  struct vdo_completion *completion;
  makeWrappedVIO(slabNumber, &completion);
  VDO_ASSERT_SUCCESS(performAction(addSlabJournalEntryAction, completion));
  UDS_FREE(completion);
}

/**
 * Test that dirty slab journals are ordered correctly.
 **/
static void testDirtySlabOrdering(void)
{
  performAddEntry(2);
  performAddEntry(3);
  performAddEntry(0);
  performAddEntry(1);

  struct block_allocator *allocator = vdo->depot->allocators[0];
  for (slab_count_t i = 0; i < 4; i++) {
    struct slab_journal *journal
      = list_first_entry(&allocator->dirty_slab_journals,
                         struct slab_journal,
                         dirty_entry);
    list_del_init(&journal->dirty_entry);
    CU_ASSERT_EQUAL(i, journal->slab->slab_number);
  }

  CU_ASSERT_TRUE(list_empty(&allocator->dirty_slab_journals));
}

/**********************************************************************/
static CU_TestInfo slabJournalTests[] = {
  { "dirty slab ordering", testDirtySlabOrdering },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo slabJournalSuite = {
  .name                     = "vdo_slab journal tests (SlabJournal_t3)",
  .initializerWithArguments = NULL,
  .initializer              = slabJournalTestInitialization,
  .cleaner                  = tearDownVDOTest,
  .tests                    = slabJournalTests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &slabJournalSuite;
}
