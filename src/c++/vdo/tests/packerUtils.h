/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef PACKER_UTILS_H
#define PACKER_UTILS_H

#include "types.h"

#include "asyncVIO.h"

/**
 * Flush the packer.
 **/
void requestFlushPacker(void);

/**
 * Setup hook to latch a VIO arriving at the compressor.
 * Note that this overrides CallbackEnqueueHook in asyncLayer.
 **/
void setupCompressorLatch(void);

/**
 * Tear down the hook that latches a VIO arriving at the compressor.
 **/
void tearDownCompressorLatch(void);

/**
 * Check if a VIO is about to enter the packer.
 *
 * Implements WrapCondition.
 */
bool isLeavingCompressor(struct vdo_completion *completion)
  __attribute__((warn_unused_result));

/**
 * Wait for a VIO to get into the compressor.
 **/
void waitForVIOLatchesAtCompressor(void);

/**
 * Release the VIO that was latched by waitForVIOLatchesAtCompressor().
 **/
void releaseVIOLatchedInCompressor(void);

/**
 * Set up notification for VIOs arriving at the packer.
 * Note that this overrides the CallbackEnqueueHook in asyncLayer.
 **/
void setupPackerNotification(void);

/**
 * Tear down notification for VIOs arriving at the packer.
 * Note that this clears the CallbackEnqueueHook in asyncLayer.
 **/
void tearDownPackerNotification(void);

/**
 * Wait for any data_vio to arrive at the packer.
 **/
void waitForDataVIOToReachPacker(void);

/**
 * Prevent any DataVIOs from reaching the packer by always claiming their
 * data doesn't compress.
 **/
void preventPacking(void);

/**
 * Restore normal compression behavior.
 **/
void restorePacking(void);

#endif /* not PACKER_UTILS_H */
