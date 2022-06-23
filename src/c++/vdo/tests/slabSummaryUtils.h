/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef SLAB_SUMMARY_UTILS_H
#define SLAB_SUMMARY_UTILS_H

#include "slab-summary.h"

/**
 * A completion for updating a slab summary.
 **/
typedef struct {
  struct vdo_completion     completion;
  struct waiter             waiter;
  struct slab_summary_zone *summaryZone;
  slab_count_t              slabNumber;
  block_count_t             slabOffset;
  block_count_t             freeBlocks;
  size_t                    freeBlockHint;
  tail_block_offset_t       tailBlockOffset;
  bool                      loadRefCounts;
  bool                      isClean;
  struct slab_status       *statuses;
  bool                      shouldSignal;
  bool                      wasQueued;
} SlabSummaryClient;

/**
 * Return the client from a waiter.
 *
 * @param waiter  The waiter
 *
 * @return The client
 **/
__attribute__((warn_unused_result))
static inline SlabSummaryClient *
waiterAsSlabSummaryClient(struct waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }
  return container_of(waiter, SlabSummaryClient, waiter);
}

/**
 * Cast a vdo_completion into the test client.
 *
 * @param completion The vdo_completion to cast
 *
 * @return the wrapping completion
 **/
static inline SlabSummaryClient *
completionAsSlabSummaryClient(struct vdo_completion *completion)
{
  vdo_assert_completion_type(completion->type, VDO_TEST_COMPLETION);
  return container_of(completion, SlabSummaryClient, completion);
}

/**
 * Initialize a test client.
 *
 * @param client       The client to initialize
 * @param id           The indentifier of the client
 * @param summaryZone  The slab summary to be updated
 **/
void initializeSlabSummaryClient(SlabSummaryClient        *client,
                                 size_t                    id,
                                 struct slab_summary_zone *summaryZone);

/**
 * VDO action wrapper for updateSlabSummaryEntry().
 *
 * @param completion    A SlabSummaryClient as a completion
 **/
void doUpdateSlabSummaryEntry(struct vdo_completion *completion);

/**
 * Launch a slab summary update.
 *
 * @param client  The slab summary client to launch
 **/
void launchUpdateSlabSummaryEntry(SlabSummaryClient *client);

/**
 * Launch a slab summary update and wait for the entry to have been queued.
 *
 * @param client  The slab summary client to launch and wait on
 **/
void enqueueUpdateSlabSummaryEntry(SlabSummaryClient *client);

/*
 * Perform a slab summary update for a slab using the client wrapper.
 *
 * @param summaryZone      The slab_summary_zone
 * @param slabNumber       The slab number to update
 * @param tailBlockOffset  The new offset of the slab journal tail block
 * @param loadRefCounts    Whether refCounts should be loaded from the layer
 * @param isClean          Whether the slab is clean
 * @param freeBlocks       The number of free blocks
 *
 * return VDO_SUCCESS or an error code
 */
int performSlabSummaryUpdate(struct slab_summary_zone *summaryZone,
                             slab_count_t              slabNumber,
                             tail_block_offset_t       tailBlockOffset,
                             bool                      loadRefCounts,
                             bool                      isClean,
                             block_count_t             freeBlocks);

/**
 * Attempt to drain the slab summary. This method assumes there is only one
 * physical zone.
 *
 * @param summary  The summary to drain
 *
 * @return VDO_SUCCESS or an error
 **/
int drainSlabSummary(struct slab_summary *summary)
  __attribute__((warn_unused_result));

/**
 * Wait for the slab summary to complete writing by draining it if it is not
 * already quiescent. This method assumes there is only one physical zone.
 *
 * @param summary          The summary to close
 *
 * @return VDO_SUCCESS or an error
 **/
int closeSlabSummary(struct slab_summary *summary)
  __attribute__((warn_unused_result));

#endif // SLAB_SUMMARY_UTILS_H
