/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef LATCH_UTILS_H
#define LATCH_UTILS_H

#include <linux/list.h>

#include "types.h"

#include "asyncVIO.h"
#include "mutexUtils.h"

typedef struct {
  struct list_head         latch_entry;
  physical_block_number_t  pbn;
  struct vio              *vio;
} VIOLatch;

/**
 * A function to be run before attempting to latch a VIO or after it has been
 * blocked.
 *
 * @param vio  The vio being considered
 **/
typedef void LatchHook(struct vio *vio);

/**
 * A function to examine a VIOLatch.
 *
 * @param latch  A VIOLatch
 *
 * @return true if the examination is done
 **/
typedef bool LatchExaminer(VIOLatch *latch);

/**
 * Initialize the latch utils.
 *
 * @param expectedEntries  The expected number of concurrent latches (0 if
 *                         unknown)
 * @param condition        The WaitCondition for deciding whether to latch
 *                         a VIO
 * @param attemptHook      A function to call just before a VIO is checked
 *                         for blocking (may be NULL)
 * @param latchedHook      A function to call when a VIO is blocked (may be
 *                         NULL)
 **/
void initializeLatchUtils(size_t         expectedEntries,
                          WaitCondition *condition,
                          LatchHook     *attemptHook,
                          LatchHook     *latchedHook);

/**
 * Clean up latch utilities.
 **/
void tearDownLatchUtils(void);

/**
 * Set a latch.
 *
 * @param pbn  The physical block number of a VIO to block when the
 *             WaitCondition is met
 **/
void setLatch(physical_block_number_t pbn);

/**
 * Clear a latch. If a VIO was latched, it will be released.
 *
 * @param pbn  The physical block number of the latch to clear
 **/
void clearLatch(physical_block_number_t pbn);

/**
 * Wait for a VIO operating on the specified physical block to be blocked.
 *
 * @param pbn  The PBN to wait on
 **/
void waitForLatchedVIO(physical_block_number_t pbn);

/**
 * Wait for a VIO operating on the specified physical block to be blocked,
 * and then release it. The latch will no longer be set.
 *
 * @param pbn  The PBN to wait on and release
 **/
void releaseLatchedVIO(physical_block_number_t pbn);

/**
 * Release a VIO if it is latched, but do not block. If a VIO is released,
 * the latch will no longer be set.
 *
 * @param pbn  The PBN of the latch to release
 *
 * @return <code>true</code> if a VIO was released
 **/
bool releaseIfLatched(physical_block_number_t pbn);

/**
 * Apply an examiner to each of the latches in the latch list until either
 * the examiner returns true, or there are no more latches to examine.
 *
 * @param examiner  The examiner to apply
 **/
void examineLatches(LatchExaminer *examiner);

#endif /* LATCH_UTILS_H */
