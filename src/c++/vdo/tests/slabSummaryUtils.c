/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "slabSummaryUtils.h"

#include "slab-depot.h"

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
  vdo_fail_completion(&client->completion, result);
}

/**********************************************************************/
void initializeSlabSummaryClient(SlabSummaryClient *client, slab_count_t slabNumber)
{
  vdo_initialize_completion(&client->completion, vdo, VDO_TEST_COMPLETION);
  memset(&client->waiter, 0x0, sizeof(client->waiter));
  client->slab            = (struct vdo_slab) {
    .slab_number = slabNumber,
    .allocator   = &vdo->depot->allocators[slabNumber % vdo->thread_config.physical_zone_count],
  };
  client->waiter.callback = slabSummaryUpdated;
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
  vdo_update_slab_summary_entry(&client->slab,
                                &client->waiter,
                                client->tailBlockOffset,
                                client->loadRefCounts,
                                client->isClean,
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
int performSlabSummaryUpdate(slab_count_t         slabNumber,
                             tail_block_offset_t  tailBlockOffset,
                             bool                 loadRefCounts,
                             bool                 isClean,
                             block_count_t        freeBlocks)
{
  SlabSummaryClient client;
  initializeSlabSummaryClient(&client, slabNumber);
  client.tailBlockOffset = tailBlockOffset;
  client.loadRefCounts   = loadRefCounts;
  client.isClean         = isClean;
  client.freeBlocks      = freeBlocks;
  return performAction(doUpdateSlabSummaryEntry, &client.completion);
}

/**********************************************************************/
static void drainSlabSummaryAction(struct vdo_completion *completion)
{
  struct block_allocator *allocator = completion->parent;
  if (checkQuiescence && vdo_is_state_quiescent(&allocator->summary_state)) {
    vdo_finish_completion(completion);
    return;
  }

  vdo_start_draining(&allocator->summary_state,
                     VDO_ADMIN_STATE_SAVING,
                     completion,
                     initiate_summary_drain);
}

/**********************************************************************/
static int performDrain(struct block_allocator *allocator)
{
  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  completion.parent = allocator;
  completion.callback_thread_id = allocator->thread_id;
  return performAction(drainSlabSummaryAction, &completion);
}

/**********************************************************************/
int drainSlabSummary(struct block_allocator *allocator)
{
  checkQuiescence = false;
  return performDrain(allocator);
}

/**********************************************************************/
int closeSlabSummary(struct block_allocator *allocator)
{
  checkQuiescence = true;
  return performDrain(allocator);
}
