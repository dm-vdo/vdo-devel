/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "encodings.h"
#include "slab-depot.h"
#include "vdo.h"
#include "wait-queue.h"

#include "slabSummaryReader.h"
#include "userVDO.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  INITIAL_ZONES = 3,
};

static zone_count_t           zone;
static struct vdo_slab        slab;
static struct waiter          waiter;
static struct vdo_completion *updateCompletion;

/**
 * Set up a slab_summary and layers for test purposes.
 **/
static void initializeSlabSummaryT2(void)
{
  TestParameters testParameters = {
    .logicalThreadCount  = 1,
    .physicalThreadCount = INITIAL_ZONES,
    .hashZoneThreadCount = 1,
    .noIndexRegion       = true,
  };
  initializeVDOTest(&testParameters);
  vdo->depot->hint_shift = vdo_get_slab_summary_hint_shift(23);
}

/**********************************************************************/
static void updateNextSlab(struct waiter *waiter, void *context)
{
  int result = *((int *) context);
  if (result == -1) {
    // This is the first time.
    slab.slab_number = 0;
  } else {
    vdo_set_completion_result(updateCompletion, result);
    slab.slab_number++;
  }

  if (slab.slab_number == MAX_VDO_SLABS) {
    vdo_start_draining(&slab.allocator->summary_state,
                       VDO_ADMIN_STATE_SAVING,
                       updateCompletion,
                       initiate_summary_drain);
    return;
  }

  bool inZone = ((slab.slab_number % INITIAL_ZONES) == zone);
  slab.allocator = &vdo->depot->allocators[zone];
  vdo_update_slab_summary_entry(&slab,
                                waiter,
                                (inZone ? (slab.slab_number & 0xff) : 0),
                                inZone,
                                !inZone,
                                (zone << vdo->depot->hint_shift));
}

/**********************************************************************/
static void updateAllocatorSummaryAction(struct vdo_completion *completion)
{
  updateCompletion = completion;
  waiter.next_waiter = NULL;
  waiter.callback = updateNextSlab;
  int result = -1;
  updateNextSlab(&waiter, &result);
}

/**********************************************************************/
static void loadSummary(struct vdo_completion *completion)
{
  load_slab_summary(vdo->depot, completion);
}

/**********************************************************************/
static void testMultipleZones(void)
{
  for (zone = 0; zone < INITIAL_ZONES; zone++) {
    performSuccessfulAction(updateAllocatorSummaryAction);
  }

  // write out the summary
  suspendVDO(true);

  // Check that the user space tools can also read the summary.
  UserVDO *userVDO;
  VDO_ASSERT_SUCCESS(loadVDO(layer, true, &userVDO));
  struct slab_summary_entry *entries;
  VDO_ASSERT_SUCCESS(readSlabSummary(userVDO, &entries));
  freeUserVDO(&userVDO);

  // Clear the summary.
  memset(vdo->depot->summary_entries,
         0,
         MAXIMUM_VDO_SLAB_SUMMARY_ENTRIES * sizeof(struct slab_summary_entry));
  resumeVDO(vdo->device_config->owning_target);

  // Read it back in.
  vdo->depot->old_zone_count = vdo->depot->zone_count;
  vdo->depot->zone_count = MAX_VDO_PHYSICAL_ZONES;
  performSuccessfulAction(loadSummary);

  struct slab_summary_entry *entry = vdo->depot->summary_entries;
  for (zone_count_t zone = 0; zone < MAX_VDO_PHYSICAL_ZONES; zone++) {
    for (slab_count_t s = 0; s < MAX_VDO_SLABS; s++, entry++) {
      CU_ASSERT_EQUAL(s & 0xff, entry->tail_block_offset);
      CU_ASSERT_EQUAL((s % INITIAL_ZONES) & 0x7f, entry->fullness_hint);
      CU_ASSERT_TRUE(entry->is_dirty);
      CU_ASSERT_TRUE(entry->load_ref_counts);

      if (zone == 0) {
        CU_ASSERT_EQUAL(*((uint8_t *) entry), *((uint8_t *) &entries[s]));
      }
    }
  }

  UDS_FREE(entries);

  vdo->depot->zone_count = vdo->depot->old_zone_count;
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test multiple zone save and load", testMultipleZones },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "multi-zone slab_summary tests (SlabSummary_t2)",
  .initializer = initializeSlabSummaryT2,
  .cleaner     = tearDownVDOTest,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
