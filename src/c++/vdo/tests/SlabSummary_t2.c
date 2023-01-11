/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "read-only-notifier.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vdo-layout.h"
#include "wait-queue.h"

#include "slabSummaryReader.h"
#include "userVDO.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  INITIAL_ZONES = 3,
};

static struct fixed_layout       *layout;
static struct partition          *partition;
static struct slab_summary_zone  *summaryZone;

static struct read_only_notifier *readOnlyNotifier = NULL;
static struct slab_summary       *summary          = NULL;
static struct thread_config      *threadConfig     = NULL;
static struct waiter              waiter;
static slab_count_t               slabCount;
static struct vdo_completion     *updateCompletion;

/**
 * Set up a slab_summary and layers for test purposes.
 **/
static void initializeSlabSummaryT2(void)
{
  TestParameters testParameters = {
    .mappableBlocks      = VDO_SLAB_SUMMARY_BLOCKS,
    .logicalThreadCount  = 1,
    .physicalThreadCount = MAX_VDO_PHYSICAL_ZONES,
    .hashZoneThreadCount = 1,
    .noIndexRegion       = true,
  };
  initializeBasicTest(&testParameters);

  for (zone_count_t z = 0; z < MAX_VDO_PHYSICAL_ZONES; z++) {
    VDO_ASSERT_SUCCESS(vdo_make_default_thread(vdo,
                                               vdo->thread_config->physical_threads[z]));
  }

  VDO_ASSERT_SUCCESS(vdo_make_fixed_layout(VDO_SLAB_SUMMARY_BLOCKS, 0, &layout));

  int result = vdo_make_fixed_layout_partition(layout,
                                               VDO_SLAB_SUMMARY_PARTITION,
                                               VDO_SLAB_SUMMARY_BLOCKS,
                                               VDO_PARTITION_FROM_BEGINNING,
                                               0);
  VDO_ASSERT_SUCCESS(result);
  VDO_ASSERT_SUCCESS(vdo_get_fixed_layout_partition(layout,
						    VDO_SLAB_SUMMARY_PARTITION,
						    &partition));
}

/**
 * Destroy a summary and its associated notifier and thread config.
 **/
static void destroySummary(void)
{
  vdo_free_slab_summary(UDS_FORGET(summary));
  vdo_free_read_only_notifier(UDS_FORGET(readOnlyNotifier));
  vdo_free_thread_config(UDS_FORGET(threadConfig));
}

/**
 * Tear down a slab_summary and its associated variables and layers.
 **/
static void tearDownSlabSummaryT2(void)
{
  destroySummary();
  vdo_free_fixed_layout(UDS_FORGET(layout));
  tearDownVDOTest();
}

/**
 * Make the summary.
 *
 * @param zones  The number of physical zones
 **/
static void makeSummary(zone_count_t zones)
{
  destroySummary();
  struct thread_count_config counts = {
    .logical_zones = 1,
    .physical_zones = zones,
    .hash_zones = 1,
  };
  VDO_ASSERT_SUCCESS(vdo_make_thread_config(counts, &threadConfig));
  VDO_ASSERT_SUCCESS(vdo_make_read_only_notifier(false,
                                                 threadConfig,
                                                 vdo,
                                                 &readOnlyNotifier));
  VDO_ASSERT_SUCCESS(vdo_make_slab_summary(vdo,
                                           partition,
                                           threadConfig,
                                           23,
                                           1 << 22,
                                           readOnlyNotifier,
                                           &summary));
}

/**
 * An action to load the slab summary.
 *
 * @param completion  The completion for the action
 **/
static void loadSlabSummaryAction(struct vdo_completion *completion)
{
  vdo_load_slab_summary(summary, VDO_ADMIN_STATE_LOADING, INITIAL_ZONES,
                        completion);
}

/**
 * Verify that summary for all active zones are correct. Also verify that
 * that the user space read_slab_summary() method reads them correctly.
 *
 * @param zones  The number of physical zones
 **/
static void verifySummary(zone_count_t zones)
{
  // Check that the user space tools can also read the summary.
  UserVDO *vdo;
  VDO_ASSERT_SUCCESS(makeUserVDO(layer, &vdo));
  vdo->states.slab_depot.zone_count = INITIAL_ZONES;
  vdo->states.layout = layout;
  struct slab_summary_entry *entries;
  VDO_ASSERT_SUCCESS(readSlabSummary(vdo, &entries));
  vdo->states.layout = NULL;
  freeUserVDO(&vdo);

  makeSummary(zones);
  performSuccessfulAction(loadSlabSummaryAction);

  for (zone_count_t zone = 0; zone < zones; zone++) {
    for (slab_count_t s = 0; s < MAX_VDO_SLABS; s++) {
      summaryZone = vdo_get_slab_summary_for_zone(summary, zone);
      struct slab_summary_entry *entry = &summaryZone->entries[s];
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
}

/**********************************************************************/
static void updateNextSlab(struct waiter *waiter, void *context)
{
  vdo_set_completion_result(updateCompletion, *((int *) context));
  if (slabCount == MAX_VDO_SLABS) {
    vdo_drain_slab_summary_zone(summaryZone, VDO_ADMIN_STATE_SAVING,
                                updateCompletion);
    return;
  }

  zone_count_t    zone       = summaryZone->zone_number;
  bool            inZone     = ((slabCount % INITIAL_ZONES) == zone);
  slab_count_t    slabNumber = slabCount++;
  vdo_update_slab_summary_entry(summaryZone, waiter, slabNumber,
                                (inZone ? (slabNumber & 0xff) : 0),
                                inZone, !inZone,
                                (zone << summaryZone->summary->hint_shift));
}

/**********************************************************************/
static void updateSummaryZoneAction(struct vdo_completion *completion)
{
  updateCompletion = completion;
  waiter.next_waiter = NULL;
  waiter.callback = updateNextSlab;
  slabCount = 0;
  int result = VDO_SUCCESS;
  updateNextSlab(&waiter, &result);
}

/**********************************************************************/
static void testMultipleZones(void)
{
  makeSummary(INITIAL_ZONES);
  for (zone_count_t zone = 0; zone < INITIAL_ZONES; zone++) {
    summaryZone = vdo_get_slab_summary_for_zone(summary, zone);
    performSuccessfulAction(updateSummaryZoneAction);
  }

  verifySummary(2);
  verifySummary(MAX_VDO_PHYSICAL_ZONES);
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "test multiple zone save and load", testMultipleZones },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo suite = {
  .name        = "multi-zone slab_summary tests (SlabSummary_t2)",
  .initializer = initializeSlabSummaryT2,
  .cleaner     = tearDownSlabSummaryT2,
  .tests       = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
