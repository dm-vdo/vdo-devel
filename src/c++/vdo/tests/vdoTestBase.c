/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "vdoTestBase.h"

#include <stdlib.h>

#include <linux/kobject.h>

#include "memory-alloc.h"
#include "syscalls.h"

#include "admin-state.h"
#include "block-map.h"
#include "completionUtils.h"
#include "device-config.h"
#include "device-registry.h"
#include "instance-number.h"
#include "num-utils.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab.h"
#include "status-codes.h"
#include "status-codes.h"
#include "super-block.h"
#include "vdo-component-states.h"
#include "vdo-component.h"
#include "vdo-resize-logical.h"
#include "vdo-resize.h"
#include "vdo-resume.h"
#include "vdo-suspend.h"
#include "vdo.h"
#include "volume-geometry.h"

#include "blockMapUtils.h"
#include "userVDO.h"
#include "vdoConfig.h"

#include "albtest.h"
#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "ramLayer.h"
#include "testBIO.h"
#include "testParameters.h"
#include "testPrototypes.h"
#include "testUtils.h"
#include "vdoAsserts.h"

typedef struct tearDownItem TearDownItem;
struct tearDownItem {
  TearDownAction *action;
  TearDownItem   *next;
};

static PhysicalLayer     *synchronousLayer;
static bool               inRecovery;
static TearDownItem      *tearDownItems = NULL;
static TestConfiguration  configuration;
static bool               flushDone;
static struct bio        *flushBIO;

PhysicalLayer *layer;
struct vdo    *vdo;

/**********************************************************************/
void registerTearDownAction(TearDownAction *action)
{
  TearDownItem *item;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, TearDownItem, __func__, &item));

  item->action = action;
  item->next = tearDownItems;
  tearDownItems = item;
}

/**********************************************************************/
void initializeVDOTestBase(void)
{
  UDS_ASSERT_SUCCESS(vdo_register_status_codes());
  initializeMutexUtils();
  initializeCallbackWrapping();
  vdo_initialize_instance_number_tracking();
  registerTearDownAction(vdo_clean_up_instance_number_tracking);
  vdo = NULL;
}

/**********************************************************************/
void tearDownVDOTestBase(void)
{
  while (tearDownItems != NULL) {
    TearDownItem *item = tearDownItems;
    tearDownItems = tearDownItems->next;
    item->action();
    UDS_FREE(item);
  }
}

/**********************************************************************/
void clearHooks(void)
{
  clearBIOSubmitHook();
  clearLayerHooks();
}

/**********************************************************************/
PhysicalLayer *getSynchronousLayer(void)
{
  return synchronousLayer;
}

/**********************************************************************/
void formatTestVDO(void)
{
  struct index_config *indexConfig = ((configuration.indexConfig.mem == 0)
                                      ? NULL
                                      : &configuration.indexConfig);
  VDO_ASSERT_SUCCESS(formatVDO(&configuration.config,
                               indexConfig,
                               synchronousLayer));
}

/**********************************************************************/
void startQueues(void)
{
  formatTestVDO();
  startAsyncLayer(configuration, false);
}

/**********************************************************************/
void startVDO(enum vdo_state expectedState)
{
  startAsyncLayer(configuration, true);
  CU_ASSERT_EQUAL(expectedState, vdo->load_state);

  // Check that the newly loaded VDO has the expected configuration.
  // Implicitly tests that the vdo_config is correctly encoded and decoded.
  struct vdo_config *config = &configuration.config;
  CU_ASSERT_EQUAL(config->physical_blocks,
                  vdo->states.vdo.config.physical_blocks);
  CU_ASSERT_EQUAL(config->slab_size, vdo->states.vdo.config.slab_size);
  CU_ASSERT_EQUAL(config->recovery_journal_size,
                  vdo->states.vdo.config.recovery_journal_size);

  if ((expectedState == VDO_NEW) && (config->logical_blocks == 0)) {
    // The volume was just formatted to use the default logical block capacity,
    // so grab the the value it was defaulted to instead of checking it.
    config->logical_blocks = vdo->states.vdo.config.logical_blocks;
  } else {
    CU_ASSERT_EQUAL(config->logical_blocks,
                    vdo->states.vdo.config.logical_blocks);
  }
}

/**********************************************************************/
void startReadOnlyVDO(enum vdo_state expectedState)
{
  setStartStopExpectation(VDO_READ_ONLY);
  startVDO(expectedState);
}

/**********************************************************************/
void startVDOExpectError(int expectedError)
{
  setStartStopExpectation(expectedError);
  startAsyncLayer(configuration, true);
}

/**********************************************************************/
void stopVDO(void)
{
  if (vdo != NULL) {
    configuration.config = vdo->states.vdo.config;
  }

  stopAsyncLayer();
}

/**
 * A bio endio function to signal that a flush is complete.
 *
 * Implements bio_end_io_t.
 **/
static void signalFlushDone(struct bio *bio)
{
  if (bio == flushBIO) {
    signalState(&flushDone);
  }

  UDS_FREE(bio);
}

/**********************************************************************/
void crashVDO(void)
{
  flushDone = false;
  flushBIO = createFlushBIO(signalFlushDone);
  vdo_launch_flush(vdo, flushBIO);
  waitForStateAndClear(&flushDone);
  prepareToCrashRAMLayer(synchronousLayer);
  stopVDO();
  crashRAMLayer(synchronousLayer);
}

/**********************************************************************/
void assertVDOState(enum vdo_state expected)
{
  CU_ASSERT_EQUAL(vdo_get_state(vdo), expected);
}

/**********************************************************************/
TestConfiguration getTestConfig(void)
{
  return configuration;
}

/**********************************************************************/
block_count_t __must_check getPhysicalBlocksFree(void)
{
  /*
   * We can't ever shrink a volume except when resize fails, and we
   * can't allocate from the new slabs until after the resize succeeds,
   * so by getting the number of allocated blocks first, we ensure the
   * allocated count is always less than the capacity. Doing it in the
   * other order on a full volume could lose a race with a successful
   * resize, resulting in a nonsensical negative/underflow result.
   */
  block_count_t allocated = vdo_get_slab_depot_allocated_blocks(vdo->depot);
  smp_mb();
  return (vdo_get_slab_depot_data_blocks(vdo->depot) - allocated);
}

/**********************************************************************/
void restartVDO(bool format)
{
  bool    wasStarted = (vdo != NULL);
  nonce_t oldNonce   = (wasStarted ? vdo->states.vdo.nonce : 0);
  stopVDO();

  if (format) {
    formatTestVDO();
  }

  startVDO(format ? VDO_NEW : VDO_CLEAN);
  CU_ASSERT_EQUAL((format || !wasStarted),
                  (oldNonce != vdo->states.vdo.nonce));
}

/**********************************************************************/
void reloadVDO(struct device_config deviceConfig)
{
  stopVDO();
  configuration.deviceConfig = deviceConfig;
  startVDO(VDO_CLEAN);
}

/**
 * Perform common test initialization.
 **/
static void initializeTest(const TestParameters *parameters)
{
  vdo_initialize_device_registry_once();
  initialize_kernel_kobject();
  restorePacking();
  configuration = makeTestConfiguration(parameters);
  VDO_ASSERT_SUCCESS(makeRAMLayer(configuration.config.physical_blocks,
                                  !configuration.synchronousStorage,
                                  &synchronousLayer));
  initializeAsyncLayer(synchronousLayer);
  clearHooks();
  initializeDataBlocks(configuration.dataFormatter);
}

/**********************************************************************/
void initializeBasicTest(const TestParameters *parameters)
{
  initializeTest(parameters);
  startQueues();
}

/**********************************************************************/
void initializeDefaultBasicTest(void)
{
  initializeBasicTest(NULL);
}

/**********************************************************************/
void initializeVDOTest(const TestParameters *parameters)
{
  initializeTest(parameters);
  restartVDO(true);
}

/**********************************************************************/
void initializeDefaultVDOTest(void)
{
  initializeVDOTest(NULL);
}

/**********************************************************************/
void initializeTestWithSynchronousLayer(const TestParameters *parameters,
                                        PhysicalLayer        *syncLayer)
{
  vdo_initialize_device_registry_once();
  initialize_kernel_kobject();
  configuration = makeTestConfiguration(parameters);
  synchronousLayer = syncLayer;
  initializeAsyncLayer(synchronousLayer);
}

/**********************************************************************/
void tearDownVDOTest(void)
{
  clearHooks();
  stopVDO();
  destroyAsyncLayer();

  if (synchronousLayer != NULL) {
    synchronousLayer->destroy(&synchronousLayer);
  }

  tearDownDataBlocks();

  /*
   * Since data_vio_count is a global variable, changes to it can bleed across
   * tests when running with --no-fork. Therefore, we always reset it to the
   * default at the end of a test so that future test writers needn't remember
   * to do so. This is especially important since tracking down the resulting
   * hangs is tricky.
   */
  data_vio_count = MAXIMUM_VDO_USER_VIOS;
}

/**********************************************************************/
void performActionOnThreadExpectResult(vdo_action  *action,
                                       thread_id_t  threadID,
                                       int          expectedResult)
{
  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  completion.callback_thread_id = threadID;
  CU_ASSERT_EQUAL(expectedResult, performAction(action, &completion));
}

/**********************************************************************/
void performActionExpectResult(vdo_action *action, int expectedResult)
{
  performActionOnThreadExpectResult(action, 0, expectedResult);
}

/**********************************************************************/
void performSuccessfulActionOnThread(vdo_action *action, thread_id_t threadID)
{
  performActionOnThreadExpectResult(action, threadID, VDO_SUCCESS);
}

/**********************************************************************/
void performSuccessfulAction(vdo_action *action)
{
  performActionOnThreadExpectResult(action, 0, VDO_SUCCESS);
}

/**********************************************************************/
void checkVDOState(enum vdo_state expectedState)
{
  UserVDO *vdo;
  VDO_ASSERT_SUCCESS(loadVDO(synchronousLayer, false, &vdo));
  CU_ASSERT_EQUAL(expectedState, vdo->states.vdo.state);
  freeUserVDO(&vdo);
}

/**********************************************************************/
static void assertReadOnlyAction(struct vdo_completion *completion)
{
  CU_ASSERT(vdo_is_read_only(vdo->read_only_notifier));
  vdo_complete_completion(completion);
}

/**********************************************************************/
void verifyReadOnly(void)
{
  performSuccessfulAction(assertReadOnlyAction);
  setStartStopExpectation(VDO_READ_ONLY);
}

/**********************************************************************/
static void forceReadOnlyMode(struct vdo_completion *completion)
{
  vdo_enter_read_only_mode(vdo->read_only_notifier, VDO_READ_ONLY);
  vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier,
                                             completion);
  setStartStopExpectation(VDO_READ_ONLY);
}

/**********************************************************************/
void forceVDOReadOnlyMode(void)
{
  performSuccessfulAction(forceReadOnlyMode);
  checkVDOState(VDO_READ_ONLY_MODE);
}

/**********************************************************************/
void forceRebuild(void)
{
  forceVDOReadOnlyMode();
  setStartStopExpectation(VDO_READ_ONLY);
  stopVDO();
  VDO_ASSERT_SUCCESS(forceVDORebuild(synchronousLayer));
  setStartStopExpectation(VDO_SUCCESS);
}

/**********************************************************************/
void rebuildReadOnlyVDO(void)
{
  forceRebuild();
  startVDO(VDO_FORCE_REBUILD);
}

/**
 * Action to check whether the VDO is in recovery mode.
 **/
static void checkRecoveryDone(struct vdo_completion *completion)
{
  if (!vdo_in_recovery_mode(vdo)) {
    inRecovery = false;
  }
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**********************************************************************/
void waitForRecoveryDone(void)
{
  inRecovery = true;
  while (inRecovery) {
    performSuccessfulActionOnThread(checkRecoveryDone,
                                    vdo->thread_config->admin_thread);
  }
}

/**
 * A vdo_action to enable VDO compression from the request thread.
 **/
static void enableCompressionAction(struct vdo_completion *completion)
{
  vdo_set_compressing(vdo, true);
  vdo_complete_completion(completion);
}

/**
 * A vdo_action to disable VDO compression from the request thread.
 **/
static void disableCompressionAction(struct vdo_completion *completion)
{
  vdo_set_compressing(vdo, false);
  vdo_complete_completion(completion);
}

/**********************************************************************/
void performSetVDOCompressing(bool enable)
{
  performSuccessfulActionOnThread((enable
                                   ? enableCompressionAction
                                   : disableCompressionAction),
                                  vdo->thread_config->packer_thread);
}

/**********************************************************************/
block_count_t computeDataBlocksToFill(void)
{
  block_count_t dataBlocks = getPhysicalBlocksFree();
  block_count_t toWrite    = dataBlocks - computeBlockMapOverhead(dataBlocks);
  CU_ASSERT_EQUAL(toWrite, dataBlocks - computeBlockMapOverhead(toWrite));
  return toWrite;
}

/**********************************************************************/
block_count_t fillPhysicalSpace(logical_block_number_t lbn,
                                block_count_t          dataOffset)
{
  block_count_t freeBlocks;
  block_count_t blocksWritten = 0;
  while ((freeBlocks = getPhysicalBlocksFree()) > 0) {
    block_count_t currentOverhead
      = vdo_get_journal_block_map_data_blocks_used(vdo->recovery_journal);
    block_count_t allocated   = vdo_get_physical_blocks_allocated(vdo);
    block_count_t newOverhead = computeBlockMapOverhead(freeBlocks + allocated);
    if (newOverhead < currentOverhead) {
      newOverhead = currentOverhead;
    }
    block_count_t blocksToFill = freeBlocks - (newOverhead - currentOverhead);
    blocksToFill = min(blocksToFill, (block_count_t) MAXIMUM_VDO_USER_VIOS);
    VDO_ASSERT_SUCCESS(performIndexedWrite(lbn + blocksWritten, blocksToFill,
                                           dataOffset + blocksWritten));
    blocksWritten += blocksToFill;
  }
  return blocksWritten;
}

/**********************************************************************/
block_count_t populateBlockMapTree(void)
{
  block_count_t leafPages
    = vdo_compute_block_map_page_count(configuration.config.logical_blocks);
  for (block_count_t i = 0; i < leafPages; i++) {
    zeroData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, VDO_SUCCESS);
    discardData(i * VDO_BLOCK_MAP_ENTRIES_PER_PAGE, 1, VDO_SUCCESS);
  }

  return getPhysicalBlocksFree();
}

/**********************************************************************/
int perform_vdo_suspend(bool save)
{
  vdo->suspend_type = (save
                       ? VDO_ADMIN_STATE_SAVING
                       : VDO_ADMIN_STATE_SUSPENDING);
  return vdo_suspend(vdo);
}

/**********************************************************************/
int modifyCompressDedupe(bool compress, bool dedupe)
{
  char *error;
  struct device_config *old_config = vdo->device_config;
  struct device_config *config;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct device_config, __func__, &config));
  *config = *old_config;
  config->compression = compress;
  config->deduplication = dedupe;

  int result = vdo_prepare_to_modify(vdo, config, true, &error);
  if (result != VDO_SUCCESS) {
    UDS_FREE(config);
    return result;
  }

  VDO_ASSERT_SUCCESS(perform_vdo_suspend(false));

  result = vdo_preresume_internal(vdo, config, "test device");
  UDS_FREE((result == VDO_INVALID_ADMIN_STATE) ? config : old_config);
  if (result == VDO_SUCCESS) {
    configuration.config = vdo->states.vdo.config;
  }

  return result;
}

/**********************************************************************/
static int modifyVDO(block_count_t logicalSize,
		     block_count_t physicalSize,
		     bool          save)
{
  char *error;
  struct device_config *old_config = vdo->device_config;
  struct device_config *config;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct device_config, __func__, &config));
  *config = *old_config;
  config->logical_blocks = logicalSize;
  config->physical_blocks = physicalSize;

  int result = vdo_prepare_to_modify(vdo, config, true, &error);
  if (result != VDO_SUCCESS) {
    UDS_FREE(config);
    return result;
  }

  VDO_ASSERT_SUCCESS(perform_vdo_suspend(save));

  block_count_t oldSize = synchronousLayer->getBlockCount(synchronousLayer);
  if (oldSize < physicalSize) {
    VDO_ASSERT_SUCCESS(resizeRAMLayer(synchronousLayer, physicalSize));
  }

  result = vdo_preresume_internal(vdo, config, "test device");
  UDS_FREE((result == VDO_INVALID_ADMIN_STATE) ? config : old_config);
  if (result == VDO_SUCCESS) {
    configuration.config = vdo->states.vdo.config;
  }

  return result;
}

/**********************************************************************/
int growVDOLogical(block_count_t newSize, bool save)
{
  return modifyVDO(newSize, vdo->device_config->physical_blocks, save);
}

/**********************************************************************/
void growVDOPhysical(block_count_t newSize, int expectedResult)
{
  bool readOnly = false;
  block_count_t oldSize = configuration.config.physical_blocks;

  if (expectedResult == VDO_READ_ONLY) {
    readOnly = true;
    expectedResult = VDO_SUCCESS;
  }

  CU_ASSERT_EQUAL(expectedResult,
                  modifyVDO(vdo->device_config->logical_blocks, newSize,
                            false));

  if (readOnly) {
    verifyReadOnly();
    configuration.config.physical_blocks = oldSize;
  }
}

/**********************************************************************/
void performSuccessfulSuspendAndResume(bool save)
{
  VDO_ASSERT_SUCCESS(perform_vdo_suspend(save));
  VDO_ASSERT_SUCCESS(vdo_preresume_internal(vdo, vdo->device_config,
					    "test name"));
}

/**********************************************************************/
void addSlabs(slab_count_t slabCount)
{
  block_count_t  newSize = (vdo->states.vdo.config.physical_blocks
                         + (vdo->depot->slab_config.slab_blocks * slabCount));
  growVDOPhysical(newSize, VDO_SUCCESS);
}

/**********************************************************************/
void assertNotInIndexRegion(physical_block_number_t pbn)
{
  CU_ASSERT((pbn < configuration.indexRegionStart)
            || (pbn >= configuration.vdoRegionStart));
}
