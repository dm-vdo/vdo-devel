/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef BLOCK_ALLOCATOR_UTILS_H
#define BLOCK_ALLOCATOR_UTILS_H

#include "encodings.h"
#include "slab-depot.h"
#include "types.h"

#include "vdoAsserts.h"

struct block_allocator;

/**
 * Reserve a specified number of VIOs from an allocator's vio pool.
 *
 * @param allocator  The allocator to reserve from
 * @param count      The number of VIOs to reserve (must be smaller than the pool size)
 **/
void reserveVIOsFromPool(struct block_allocator *allocator, size_t count);

/**
 * Return the VIOs reserved by reserveVIOsFromPool() to their pool
 **/
void returnVIOsToPool(void);

/**
 * Check whether two journal points are equal
 *
 * @param a  The first journal point
 * @param b  The second journal point
 **/
static inline bool areJournalPointsEqual(struct journal_point a, struct journal_point b)
{
  return ((a.sequence_number == b.sequence_number) && (a.entry_count == b.entry_count));
}

/**
 * getReferenceStatus() - Get the reference status of a block.
 *
 * @param slab       The slab which owns the pbn
 * @param pbn        The physical block number
 * @param statusPtr  Where to put the status of the block
 *
 * Return: A success or error code, specifically: VDO_OUT_OF_RANGE if the pbn is out of range
 **/
int getReferenceStatus(struct vdo_slab *slab,
                       physical_block_number_t pbn,
                       enum reference_status *statusPtr);

/**
 * Check whether two slabs have equivalent reference counts.
 *
 * @param slabA  The first slab to compare
 * @param slabB  The second slab to compare
 *
 * @return true if the reference counters of the two slabs are equivalent
 **/
bool slabsHaveEquivalentReferenceCounts(struct vdo_slab *slabA, struct vdo_slab *slabB);

/**
 * Reset all reference counts back to RS_FREE.
 *
 * @param slab The slab to reset
 **/
void resetReferenceCounts(struct vdo_slab *slab);

/**
 * Count all unreferenced blocks in a range [start_block, end_block) of physical block numbers.
 *
 * @param slab       The slab to scan
 * @param start_pbn  The physical block number at which to start scanning (included in the scan)
 * @param end_pbn    The physical block number at which to stop scanning (excluded from the scan)
 *
 * @return: The number of unreferenced blocks
 **/
block_count_t countUnreferencedBlocks(struct vdo_slab *slab,
                                      physical_block_number_t start,
                                      physical_block_number_t end);

#endif // BLOCK_ALLOCATOR_UTILS_H
