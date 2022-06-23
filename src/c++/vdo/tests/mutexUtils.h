/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef MUTEX_UTILS_H
#define MUTEX_UTILS_H

#include "types.h"

#include "asyncVIO.h"

/**
 * A function which is to be run while holding the mutex.
 *
 * @param context  The context for the method
 *
 * @return <code>true</code> if a broadcast should be sent
 **/
typedef bool LockedMethod(void *context);

/**
 * A function which checks a condition while holding the mutex.
 *
 * @param context  The context for the check
 *
 * @return <code>true</code> if the condition being waited for has occurred
 **/
typedef bool WaitCondition(void *context);

/**
 * A function to decide whether or not to block a completion
 *
 * @param completion  The vio to consider
 * @param context     The context for the check
 *
 * @return <code>true</code> if the completion should be blocked
 **/
typedef bool BlockCondition(struct vdo_completion *completion, void *context);

/**
 * Initialize the mutex and condition. This function should only be called
 * from initializeVDOTestBase().
 **/
void initializeMutexUtils(void);

/**
 * Lock the mutex. For most uses, runLocked() should be used, but for methods
 * which need finer grained control of locked and unlocked blocks of code, this
 * may be a better choice.
 **/
void lockMutex(void);

/**
 * Unlock the mutex locked by lockMutex().
 **/
void unlockMutex(void);

/**
 * Run a method while holding the mutex. If the method returns true, the
 * condition will get a broadcast.
 *
 * @param method   The method to run
 * @param context  The context to supply to the method
 *
 * @return The result of the method
 **/
bool runLocked(LockedMethod *method, void *context);

/**
 * While holding the mutex, set a state to true, and broadcast.
 *
 * @param state  A pointer to the boolean which will be set to true
 **/
void signalState(bool *state);

/**
 * While holding the mutex, set a state to false.
 *
 * @param state  A pointer to the boolean which will be set to true
 **/
void clearState(bool *state);

/**
 * Broadcast a notification on the condition.
 **/
void broadcast(void);

/**
 * Acquire the mutex and check whether a condition holds.
 *
 * @param waitCondition  A function which should return true once the condition
 *                       pertains
 * @param context        The context for the WaitCondition
 *
 * @return Whether or not the condition holds
 **/
bool checkCondition(WaitCondition *waitCondition, void *context)
  __attribute__((warn_unused_result));

/**
 * Wait under the mutex until a condition holds.
 *
 * @param waitCondition  A function which should return true once the condition
 *                       pertains
 * @param context        The context for the WaitCondition
 **/
void waitForCondition(WaitCondition *waitCondition, void *context);

/**
 * Wait under the mutex until a condition holds, then execute a method while
 * still holding the mutex. If the method returns true, broadcast on the
 * condition before releasing the mutex.
 *
 * @param waitCondition  A function which should return true once the condition
 *                       pertains
 * @param method         The method to run once the condition occurs
 * @param context        The context for the WaitCondition and method
 **/
void runOnCondition(WaitCondition *waitCondition,
                    LockedMethod  *method,
                    void          *context);

/**
 * Check under the mutex if a condition holds, and execute a method while still
 * holding the mutex if it does. If the method returns true, broadcast on the
 * condition before releasing the mutex.
 *
 * @param waitCondition  A function which should return true if the condition
 *                       pertains
 * @param method         The method to run if the condition has occured
 * @param context        The context for the WaitCondition and method
 *
 * @return <code>true</code> if the condition pertains
 **/
bool runIfCondition(WaitCondition *waitCondition,
                    LockedMethod  *method,
                    void          *context);
/**
 * Check whether a state has occurred.
 *
 * @param state  The state to check
 *
 * @return <code>true</code> if the state has occured
 **/
bool checkState(bool *state)
  __attribute__((warn_unused_result));

/**
 * Wait for a state to occur. The method will return once the state variable
 * becomes true.
 *
 * @param state  A pointer to boolean state variable
 **/
void waitForState(bool *state);

/**
 * Wait for a state to occur. This method will not return until the state
 * variable becomes true. The variable will be cleared while holding the mutex.
 *
 * @param state  A pointer to a boolean state variable
 **/
void waitForStateAndClear(bool *state);

/**
 * Wait for a pointer not to be NULL.
 *
 * @param ptr  A pointer to the pointer to wait on
 **/
void waitForNotNull(void **ptr);

/**
 * Block a vio if it meets a specified condition.
 *
 * @param vio             The VIO to check
 * @param blockCondition  A function to check whether the VIO should be blocked
 * @param context         The context for the blockCondition
 * @param notify          Broadcast to the condition if <code>true</code> and
 *                        the VIO was blocked
 **/
void blockVIOOnCondition(struct vio     *vio,
                         BlockCondition  blockCondition,
                         void           *context,
                         bool            notify);

/**
 * Add a completion enqueue hook to block a vio.
 *
 * @param blockCondition  A function to check whether a VIO should be blocked
 * @param notify          If <code>true</code>, broadcast to the condition when
 *                        a VIO is blocked
 * @param takeOut         If <code>true</code>, take out the hook once a VIO
 *                        has been blocked
 **/
void addBlockVIOCompletionEnqueueHook(BlockCondition *condition,
                                      bool            notify,
                                      bool            takeOut);

/**
 * Set a completion enqueue hook to block a vio.
 *
 * @param blockCondition  A function to check whether a VIO should be blocked
 * @param notify          If <code>true</code>, broadcast to the condition when
 *                        a VIO is blocked
 * @param takeOut         If <code>true</code>, take out the hook once a VIO
 *                        has been blocked
 **/
void setBlockVIOCompletionEnqueueHook(BlockCondition *condition,
                                      bool            notify,
                                      bool            takeOut);

/**
 * Set a bio submit hook to block a bio.
 *
 * @param blockCondition  A function to check whether a bio should be blocked
 * @param notify          If <code>true</code>, broadcast to the condition when
 *                        a bio is blocked
 * @param takeOut         If <code>true</code>, take out the hook once a bio
 *                        has been blocked
 **/
void setBlockBIO(BlockCondition *condition,
                 bool            notify,
                 bool            takeOut);

/**
 * Block a vio.
 *
 * @param vio     The VIO to check
 * @param notify  Broadcast to the condition if <code>true</code> and the VIO
 *                was blocked
 **/
void blockVIO(struct vio *vio, bool notify);

/**
 * Wait until a VIO has been blocked.
 **/
void waitForBlockedVIO(void);

/**
 * Get the vio which was blocked by blockVIOOnCondition() or blockVIO(),
 * allowing those methods to be used again.
 *
 * @return The blocked VIO or NULL if there isn't one
 **/
struct vio *getBlockedVIO(void) __attribute__((warn_unused_result));

/**
 * Release the vio which was blocked by blockVIOOnCondition() or
 * blockVIO(), and enqueue it for processing. This allows those methods to be
 * used again.
 **/
void releaseBlockedVIO(void);

/**
 * Assert that there is no blocked VIO.
 **/
void assertNoBlockedVIOs(void);

/**
 * A WaitCondition to check whether a vio is doing a metadata write.
 *
 * @param context  The vio to check
 *
 * @return <code>true</code> if the AsyncVIO is doing a metadata write
 **/
static inline bool isMetadataWriteCondition(void *context)
{
  return isMetadataWrite(context);
}

/**
 * A WaitCondition to check whether the number of threads blocked in
 * io_schedule is equal to the specified target value.
 *
 * @param context  uint32_t * containing the desired blocked thread count
 **/
bool checkBlockedThreadCount(void *context);

#endif /* MUTEX_UTILS_H */
