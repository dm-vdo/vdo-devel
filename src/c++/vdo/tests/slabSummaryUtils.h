/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef SLAB_SUMMARY_UTILS_H
#define SLAB_SUMMARY_UTILS_H

#include "slab-depot.h"

/**
 * A completion for updating a slab summary.
 **/
typedef struct {
  struct vdo_completion  completion;
  struct waiter          waiter;
  struct vdo_slab        slab;
  block_count_t          freeBlocks;
  size_t                 freeBlockHint;
  tail_block_offset_t    tailBlockOffset;
  bool                   loadRefCounts;
  bool                   isClean;
  bool                   shouldSignal;
  bool                   wasQueued;
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
  vdo_assert_completion_type(completion, VDO_TEST_COMPLETION);
  return container_of(completion, SlabSummaryClient, completion);
}

/**
 * Initialize a test client.
 *
 * @param client     The client to initialize
 * @param slabNumber The slab whose entry is to be updated
 **/
void initializeSlabSummaryClient(SlabSummaryClient *client, slab_count_t slabNumber);

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
 * @param slabNumber       The slab whose entry is to be updated
 * @param tailBlockOffset  The new offset of the slab journal tail block
 * @param loadRefCounts    Whether refCounts should be loaded from the layer
 * @param isClean          Whether the slab is clean
 * @param freeBlocks       The number of free blocks
 *
 * return VDO_SUCCESS or an error code
 */
int performSlabSummaryUpdate(slab_count_t         slabNumber,
                             tail_block_offset_t  tailBlockOffset,
                             bool                 loadRefCounts,
                             bool                 isClean,
                             block_count_t        freeBlocks);

/**
 * Attempt to drain an allocator's slab summary.
 *
 * @param allocator  The allocator whose summary is to be drained
 *
 * @return VDO_SUCCESS or an error
 **/
int drainSlabSummary(struct block_allocator *allocator)
  __attribute__((warn_unused_result));

/**
 * Wait for an allocator's slab summary to complete writing by draining it if
 * it is not already quiescent.
 *
 * @param allocator  The allocator whose summary is to be drained
 *
 * @return VDO_SUCCESS or an error
 **/
int closeSlabSummary(struct block_allocator *allocator)
  __attribute__((warn_unused_result));

#endif // SLAB_SUMMARY_UTILS_H
