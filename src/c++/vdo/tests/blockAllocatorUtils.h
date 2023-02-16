/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef BLOCK_ALLOCATOR_UTILS_H
#define BLOCK_ALLOCATOR_UTILS_H

#include "types.h"
#include "vdo-component-states.h"

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

#endif // BLOCK_ALLOCATOR_UTILS_H
