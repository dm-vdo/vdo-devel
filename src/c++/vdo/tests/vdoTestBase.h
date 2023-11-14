/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef VDO_TEST_BASE_H
#define VDO_TEST_BASE_H

#include "types.h"
#include "vdo.h"

#include "testParameters.h"

/**
 * The type for functions which can be registered to run when a test tears
 * down.
 **/
typedef void TearDownAction(void);

extern PhysicalLayer      *layer;
extern struct vdo         *vdo;
extern struct target_type *vdoTargetType;

/**
 * Register a tear down action. These actions will be run when a test tears
 * down.
 **/
void registerTearDownAction(TearDownAction *action);

/**
 * Initialize the base test infrastructure. This method should only be called
 * from the test directory initializer.
 **/
void initializeVDOTestBase(void);

/**
 * Tear down the base test infrastructure. This method should only be called
 * from the test directory destructor.
 **/
void tearDownVDOTestBase(void);

/**
 * Clear all registered hooks.
 **/
void clearHooks(void);

/**
 * Get the synchronous layer.
 *
 * @return The synchronous layer
 **/
PhysicalLayer *getSynchronousLayer(void)
  __attribute__((warn_unused_result));

/**
 * Format a VDO.
 **/
void formatTestVDO(void);

/**
 * Start the async layer queues.
 **/
void startQueues(void);

/**
 * Start the VDO and its layer.
 *
 * @param expectedState  The state expected in the VDO's super block
 **/
void startVDO(enum vdo_state expectedState);

/**
 * Start the VDO and its layer and assert that the VDO starts in read-only
 * mode.
 *
 * @param expectedState  The state expected in the VDO's super block
 **/
void startReadOnlyVDO(enum vdo_state expectedState);

/**
 * Start the VDO and its layer and assert that the start failed with the
 * specified error. Do not use this method with an expected error of
 * VDO_READ_ONLY or VDO_SUCCESS.
 *
 * @param expectedError  The expected error code from attempting to start the
 *                       VDO
 **/
void startVDOExpectError(int expectedError);

/**
 * Stop the VDO and its asynchronous layer.
 **/
void stopVDO(void);

/**
 * Flush the VDO and then stop the layer without allowing the VDO to save
 * cleanly.
 **/
void crashVDO(void);

/**
 * Assert the state of the VDO in the layer is as expected.
 *
 * @param expected  The expected state of the VDO
 **/
void assertVDOState(enum vdo_state expected);

/**
 * Get the test configuration.
 *
 * @return The current test configuration
 **/
TestConfiguration getTestConfig(void)
  __attribute__((warn_unused_result));

/**
 * Get the UDS index session.
 *
 * @return The current UDS session
 **/
struct uds_index_session **getUdsIndexSession(void)
  __attribute__((warn_unused_result));

/**
 * Get the number of unallocated physical blocks.
 *
 * @return The number of free blocks
 **/
block_count_t __must_check getPhysicalBlocksFree(void);

/**
 * Restart the VDO (or start it for the first time).
 *
 * @param format  If <code>true</code>, the VDO will be formatted first
 **/
void restartVDO(bool format);

/**
 * Restart the VDO with a new configuration
 *
 * @param deviceConfig  The new configuration for the VDO
 **/
void reloadVDO(struct device_config deviceConfig);

/**
 * The lowest level test initialization. This should only be used directly in
 * cases where fine control of the initialization is required. This function is
 * called from all of the other initializers.
 *
 * @param parameters  The test parameters
 **/
void initializeTest(const TestParameters *parameters);

/**
 * Initialize the layer queues but no VDO.
 *
 * @param parameters  The test parameters (may be NULL)
 **/
void initializeBasicTest(const TestParameters *parameters);

/**
 * Initialize the layer queues with default parameters, but don't start a VDO.
 **/
void initializeDefaultBasicTest(void);

/**
 * Initialize a test with a VDO which will be started.
 *
 * @param parameters  The test parameters (may be NULL)
 **/
void initializeVDOTest(const TestParameters *parameters);

/**
 * Initialize a test with a VDO which will be started and default parameters.
 **/
void initializeDefaultVDOTest(void);

/**
 * Initialize the async layer starting with a specified synchronous layer.
 *
 * @param parameters  The test parameters (may be NULL)
 * @param syncLayer   The synchronous layer to use
 **/
void initializeTestWithSynchronousLayer(const TestParameters *parameters,
                                        PhysicalLayer        *syncLayer);

/**
 * Clean up after a VDO test.
 **/
void tearDownVDOTest(void);

/**
 * Perform an action on a specified callback thread and assert that the result
 * is as expected.
 *
 * @param action          The action to perform
 * @param threadID        The ID of the callback thread on which the action
 *                        should run
 * @param expectedResult  The expected result
 **/
void performActionOnThreadExpectResult(vdo_action_fn action,
                                       thread_id_t   threadID,
                                       int           expectedResult);

/**
 * Perform an action and assert that the result is as expected.
 *
 * @param action          The action to perform
 * @param expectedResult  The expected result
 **/
void performActionExpectResult(vdo_action_fn action, int expectedResult);

/**
 * Perform an action on a specified callback thread and assert that it
 * succeeds.
 *
 * @param action    The action to perform
 * @param threadID  The ID of the callback thread on which the action should
 *                  run
 **/
void performSuccessfulActionOnThread(vdo_action_fn action, thread_id_t threadID);

/**
 * Perform an action and assert that it succeeds.
 *
 * @param action  The action to perform
 **/
void performSuccessfulAction(vdo_action_fn action);

/**
 * Check the state of the VDO in the super block.
 *
 * @param expectedState  The expected state of the super block
 **/
void checkVDOState(enum vdo_state expectedState);

/**
 * Verify that the VDO is read-only.
 **/
void verifyReadOnly(void);

/**
 * Change the state of the VDO to VDO_READ_ONLY_MODE and save the super block.
 **/
void forceVDOReadOnlyMode(void);

/**
 * Force the VDO to be in read-only mode, stop it, and set it to do a read-only
 * rebuild when next started.
 **/
void forceRebuild(void);

/**
 * Do a read-only rebuild of a VDO, putting it into read-only mode and stopping
 * it if necessary. The VDO will be started by this method.
 **/
void rebuildReadOnlyVDO(void);

/**
 * Wait until the VDO has exited recovery mode.
 **/
void waitForRecoveryDone(void);

/**
 * Set the compression state of the VDO.
 *
 * @param enable  <code>true</code> to enable compression
 **/
void performSetVDOCompressing(bool enable);

/**
 * Compute the number of contiguous, unique, data blocks which need to be
 * written in order to fill the VDO. This method may not work if the VDO does
 * not start empty (but it might be OK).
 *
 * @return The number of data blocks to write
 **/
block_count_t computeDataBlocksToFill(void);

/**
 * Fill all remaining physical space.
 *
 * @param lbn         The logical block number at which to start writing
 * @param dataOffset  The offset into data of the first block to write
 *
 * @return The number of blocks written
 **/
block_count_t fillPhysicalSpace(logical_block_number_t lbn,
                                block_count_t          dataOffset);

/**
 * Allocate all block map tree pages by writing and then trimming one block
 * from each leaf page.
 *
 * @return The number of free blocks once the tree is populated
 **/
block_count_t populateBlockMapTree(void);

/**
 * Load a new table generated from the specified configuration.
 *
 * @param configuration  The TestConfiguration from which to derive the new
 *                       table
 * @param target         The target associated with the new table; on error, it
 *                       is the caller's responsibility to free the target
 *
 * @return VDO_SUCCESS or an error
 **/
int loadTable(TestConfiguration configuration, struct dm_target *target);

/**
 * Ensure that the vdo has no outstanding I/O and will issue none until it is
 * resumed.
 *
 * @param save  If <code>true</code>, all dirty metadata will be flushed as
 *              well
 *
 * @return VDO_SUCCESS or an error
 **/
int suspendVDO(bool save);

int resumeVDO(struct dm_target *target);

/**
 * Modify the compress and dedupe states as if it was from the table line
 *
 * @param compress  Whether to turn compression on or off
 * @param dedupe    Whether to turn deduplication on or off
 *
 * @return VDO_SUCCESS or an error
 */
int modifyCompressDedupe(bool compress, bool dedupe);

/**
 * Increase the logical size of a VDO.
 *
 * @param newSize  The new size (in blocks)
 * @param save     Whether to save VDO state
 *
 * @return VDO_SUCCESS or an error
 **/
int growVDOLogical(block_count_t newSize, bool save)
  __attribute__((warn_unused_result));

/**
 * Increase the physical size of a VDO and the layer below it.
 *
 * @param newSize        The new size (in blocks)
 * @param expectedResult  The expected result of the growth operation
 **/
void growVDOPhysical(block_count_t newSize, int expectedResult);

/**
 * Suspend and resume the vdo without resizing anything.
 *
 * @param save  If <code>true</code>, save all dirty metadata, otherwise just
 *              suspend
 **/
void performSuccessfulSuspendAndResume(bool save);

/**
 * Add slabs to a VDO.
 *
 * @param slabCount  The number of slabs to add
 **/
void addSlabs(slab_count_t slabCount);

/**
 * Assert that a pbn is not in the index region.
 *
 * @param pbn  The pbn to check
 **/
void assertNotInIndexRegion(physical_block_number_t pbn);

/**
 * Get the super block location.
 *
 * @return The location of the super block
 **/
static inline physical_block_number_t getSuperBlockLocation(void)
{
  return getTestConfig().vdoRegionStart;
}

#endif /* VDO_TEST_BASE_H */
