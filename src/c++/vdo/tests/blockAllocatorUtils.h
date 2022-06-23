/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef BLOCK_ALLOCATOR_UTILS_H
#define BLOCK_ALLOCATOR_UTILS_H

#include "kernel-types.h"

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

#endif // BLOCK_ALLOCATOR_UTILS_H
