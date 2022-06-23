/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef RECOVERY_MODE_UTILS_H
#define RECOVERY_MODE_UTILS_H

#include "types.h"

#include "physicalLayer.h"

#include "testParameters.h"

/**
 * Initialize a VDO test with the recovery utilities.
 *
 * @param parameters  The test parameters (may be NULL)
 **/
void initializeRecoveryModeTest(const TestParameters *parameters);

/**
 * Clean up after a VDO test with the recovery utilities.
 **/
void tearDownRecoveryModeTest(void);

/**
 * Setup an asyncLayer hook to latch the first reference count write during
 * slab scrubbing. The hook unregisters itself once a reference count write is
 * latched.
 *
 * @param slabNumber  slab number to latch
 **/
void setupSlabScrubbingLatch(slab_count_t slabNumber);

/**
 * Set up hooks to latch any slab which attempts to write a reference count.
 *
 * @param slabs  The total number of slabs which may be latched
 **/
void latchAnyScrubbingSlab(slab_count_t slabs);

/**
 * Setup an asyncLayer hook to latch the first reference count read during
 * slab scrubbing. The hook unregisters itself once a reference count read is
 * latched.
 *
 * @param slabNumber  slab number to latch
 **/
void setupSlabLoadingLatch(slab_count_t slabNumber);

/**
 * Wait for a notification that the slab has been latched.
 *
 * @param slabNumber  slab number to wait for
 **/
void waitForSlabLatch(slab_count_t slabNumber);

/**
 * Block until any slab has latched.
 *
 * @param slabs  the total number of slabs
 *
 * @return The number of a latched slab
 **/
slab_count_t waitForAnySlabToLatch(slab_count_t slabs);

/**
 * Release the latched reference count write.
 *
 * @param slabNumber      requested slab to release
 **/
void releaseSlabLatch(slab_count_t slabNumber);

/**
 * Release any latched slabs and stop trying to latch any others.
 *
 * @param slabs  The number of slabs which may be latched
 **/
void releaseAllSlabLatches(slab_count_t slabs);

/**
 * Set an error result in the VIO of a latched slab.
 *
 * @param slabNumber  The slab in which to inject the error
 * @param errorCode   The error code to inject
 **/
void injectErrorInLatchedSlab(slab_count_t slabNumber, int errorCode);

#endif // RECOVERY_MODE_UTILS_H
