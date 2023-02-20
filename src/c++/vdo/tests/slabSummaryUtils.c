/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "slabSummaryUtils.h"

#include "slab-journal.h"
#include "slab-summary.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static bool checkQuiescence;

/*
 * Finish the completion to indicate that the slab summary has been updated
 * for the client.
 */
static void slabSummaryUpdated(struct waiter *waiter, void *context)
{
  SlabSummaryClient *client = waiterAsSlabSummaryClient(waiter);
  int                result = *((int *) context);
  vdo_finish_completion(&client->completion, result);
}

/**********************************************************************/
void initializeSlabSummaryClient(SlabSummaryClient        *client,
                                 size_t                    id,
                                 struct slab_summary_zone *summaryZone)
{
  vdo_initialize_completion(&client->completion, vdo, VDO_TEST_COMPLETION);
  memset(&client->waiter, 0x0, sizeof(client->waiter));
  client->waiter.callback = slabSummaryUpdated;
  client->summaryZone     = summaryZone;
  client->slabNumber      = id;
  client->slabOffset      = id;
  client->freeBlocks      = 0;
  client->freeBlockHint   = 0;
  client->tailBlockOffset = 0;
  client->loadRefCounts   = true;
  client->isClean         = true;
  client->shouldSignal    = false;
  client->wasQueued       = false;
}

/**********************************************************************/
void doUpdateSlabSummaryEntry(struct vdo_completion *completion)
{
  SlabSummaryClient *client = completionAsSlabSummaryClient(completion);
  // Capture this before the completion callback is invoked, since that may
  // allow the test thread to destroy the completion.
  bool shouldSignal = client->shouldSignal;
  vdo_update_slab_summary_entry(client->summaryZone, &client->waiter,
                                client->slabNumber, client->tailBlockOffset,
                                client->loadRefCounts, client->isClean,
                                client->freeBlocks);

  // Must only notify if the caller is using launch/wait. If the caller is
  // using performAction(), the completion may already be gone. [VDO-4965]
  if (shouldSignal) {
    signalState(&client->wasQueued);
  }
}

/**********************************************************************/
void launchUpdateSlabSummaryEntry(SlabSummaryClient *client)
{
  launchAction(doUpdateSlabSummaryEntry, &client->completion);
}

/**********************************************************************/
void enqueueUpdateSlabSummaryEntry(SlabSummaryClient *client)
{
  client->shouldSignal = true;
  launchUpdateSlabSummaryEntry(client);
  waitForState(&client->wasQueued);
  client->shouldSignal = false;
}

/**********************************************************************/
int performSlabSummaryUpdate(struct slab_summary_zone *summaryZone,
                             slab_count_t              slabNumber,
                             tail_block_offset_t       tailBlockOffset,
                             bool                      loadRefCounts,
                             bool                      isClean,
                             block_count_t             freeBlocks)
{
  SlabSummaryClient client;
  initializeSlabSummaryClient(&client, 0, summaryZone);
  client.slabNumber      = slabNumber;
  client.tailBlockOffset = tailBlockOffset;
  client.loadRefCounts   = loadRefCounts;
  client.isClean         = isClean;
  client.freeBlocks      = freeBlocks;
  return performAction(doUpdateSlabSummaryEntry, &client.completion);
}

/**********************************************************************/
static void drainSlabSummaryAction(struct vdo_completion *completion)
{
  struct slab_summary_zone *zone = completion->parent;
  if (checkQuiescence && vdo_is_state_quiescent(&zone->state)) {
    vdo_complete_completion(completion);
    return;
  }

  vdo_drain_slab_summary_zone(completion->parent, VDO_ADMIN_STATE_SAVING,
                              completion);
}

/**********************************************************************/
static int performDrain(struct slab_summary *summary)
{
  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);

  // XXX This assumes the slab summary has only one zone.
  struct slab_summary_zone *summaryZone = summary->zones[0];
  completion.parent = summaryZone;
  completion.callback_thread_id
    = summaryZone->summary_blocks[0].vio->completion.callback_thread_id;
  return performAction(drainSlabSummaryAction, &completion);
}

/**********************************************************************/
int drainSlabSummary(struct slab_summary *summary)
{
  checkQuiescence = false;
  return performDrain(summary);
}

/**********************************************************************/
int closeSlabSummary(struct slab_summary *summary)
{
  checkQuiescence = true;
  return performDrain(summary);
}
