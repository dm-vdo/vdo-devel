/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "vdoTestBase.h"

#include <stdlib.h>

#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/module.h>

#include "memory-alloc.h"
#include "syscalls.h"

#include "admin-state.h"
#include "block-map.h"
#include "completionUtils.h"
#include "constants.h"
#include "encodings.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "status-codes.h"
#include "status-codes.h"
#include "types.h"
#include "vdo.h"

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
#include "testDM.h"
#include "testParameters.h"
#include "testPrototypes.h"
#include "testUtils.h"
#include "vdoAsserts.h"

typedef struct tearDownItem TearDownItem;
struct tearDownItem {
  TearDownAction *action;
  TearDownItem   *next;
};

static PhysicalLayer      *synchronousLayer;
static bool                inRecovery;
static TearDownItem       *tearDownItems = NULL;
static TestConfiguration   configuration;
static bool                flushDone;
static struct bio         *flushBIO;
static bool                noFlushSuspend;

PhysicalLayer      *layer;
struct vdo         *vdo;
struct target_type *vdoTargetType;
int                 suspend_result;
int                 resume_result;

/**
 * Fakes from linux/module.h.
 **/
/**********************************************************************/
int dm_register_target(struct target_type *t)
{
  vdoTargetType = t;
  return VDO_SUCCESS;
}

/**********************************************************************/
void dm_unregister_target(struct target_type *t)
{
  CU_ASSERT_PTR_EQUAL(vdoTargetType, t);
  vdo_forget(vdoTargetType);
}

/**********************************************************************/
void registerTearDownAction(TearDownAction *action)
{
  TearDownItem *item;
  VDO_ASSERT_SUCCESS(vdo_allocate(1, TearDownItem, __func__, &item));

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
  initializeDM();
  VDO_ASSERT_SUCCESS(vdo_module_initialize());
  registerTearDownAction(vdo_module_exit);
  vdo = NULL;
}

/**********************************************************************/
void tearDownVDOTestBase(void)
{
  while (tearDownItems != NULL) {
    TearDownItem *item = tearDownItems;
    tearDownItems = tearDownItems->next;
    item->action();
    vdo_free(item);
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
  VDO_ASSERT_SUCCESS(formatVDO(&configuration.config,
                               &configuration.indexConfig,
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
    // so grab the value it was defaulted to instead of checking it.
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

  vdo_free(bio);
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
  configuration.config.logical_blocks = deviceConfig.logical_blocks;
  startVDO(VDO_CLEAN);
}

/**
 * Perform common test initialization.
 **/
void initializeTest(const TestParameters *parameters)
{
  vdo_initialize_device_registry_once();
  initialize_kernel_kobject();
  restorePacking();
  configuration = makeTestConfiguration(parameters);
  if (configuration.backingFile != NULL) {
    makeRAMLayerFromFile(configuration.backingFile,
                         !configuration.synchronousStorage,
                         &synchronousLayer);
  } else {
    VDO_ASSERT_SUCCESS(makeRAMLayer(configuration.config.physical_blocks,
                                    !configuration.synchronousStorage,
                                    &synchronousLayer));
  }
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
  restartVDO((parameters == NULL) || (parameters->backingFile == NULL));
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
void performActionOnThreadExpectResult(vdo_action_fn action,
                                       thread_id_t   threadID,
                                       int           expectedResult)
{
  struct vdo_completion completion;
  vdo_initialize_completion(&completion, vdo, VDO_TEST_COMPLETION);
  completion.callback_thread_id = threadID;
  CU_ASSERT_EQUAL(expectedResult, performAction(action, &completion));
}

/**********************************************************************/
void performActionExpectResult(vdo_action_fn action, int expectedResult)
{
  performActionOnThreadExpectResult(action, 0, expectedResult);
}

/**********************************************************************/
void performSuccessfulActionOnThread(vdo_action_fn action, thread_id_t threadID)
{
  performActionOnThreadExpectResult(action, threadID, VDO_SUCCESS);
}

/**********************************************************************/
void performSuccessfulAction(vdo_action_fn action)
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
  CU_ASSERT(vdo_is_read_only(vdo));
  vdo_finish_completion(completion);
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
  vdo_enter_read_only_mode(vdo, VDO_READ_ONLY);
  vdo_wait_until_not_entering_read_only_mode(completion);
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

  vdo_finish_completion(completion);
}

/**********************************************************************/
void waitForRecoveryDone(void)
{
  inRecovery = true;
  while (inRecovery) {
    performSuccessfulActionOnThread(checkRecoveryDone, vdo->thread_config.admin_thread);
  }
}

/**
 * A vdo_action_fn to enable VDO compression from the request thread.
 **/
static void enableCompressionAction(struct vdo_completion *completion)
{
  vdo_set_compressing(vdo, true);
  vdo_finish_completion(completion);
}

/**
 * A vdo_action_fn to disable VDO compression from the request thread.
 **/
static void disableCompressionAction(struct vdo_completion *completion)
{
  vdo_set_compressing(vdo, false);
  vdo_finish_completion(completion);
}

/**********************************************************************/
void performSetVDOCompressing(bool enable)
{
  performSuccessfulActionOnThread((enable
                                   ? enableCompressionAction
                                   : disableCompressionAction),
                                  vdo->thread_config.packer_thread);
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
static void addString(char **arg, const char *s)
{
  CU_ASSERT(asprintf(arg, "%s", s) != -1);
}

/**********************************************************************/
static void addUInt32(char **arg, uint32_t u)
{
  CU_ASSERT(asprintf(arg, "%u", u) != -1);
}

/**********************************************************************/
static void addUInt64(char **arg, uint64_t u)
{
  CU_ASSERT(asprintf(arg, "%" PRIu64, u) != -1);
}

/**********************************************************************/
static void addCompressionType(char **arg, const char *s, int32_t d)
{
  CU_ASSERT(asprintf(arg, "%s:%d", s, d) != -1);
}

/**********************************************************************/
static TestConfiguration fixThreadCounts(TestConfiguration configuration)
{
  struct thread_count_config *threads = &configuration.deviceConfig.thread_counts;
  if (threads->logical_zones + threads->physical_zones + threads->hash_zones > 0) {
    if (threads->logical_zones == 0) {
      threads->logical_zones = 1;
    }

    if (threads->physical_zones == 0) {
      threads->physical_zones = 1;
    }

    if (threads->hash_zones == 0) {
      threads->hash_zones = 1;
    }
  }

  return configuration;
}

/**********************************************************************/
static int makeTableLine(TestConfiguration configuration, char **argv)
{
  int argc = 0;

  addString(&argv[argc++], "V4");
  addString(&argv[argc++], getTestIndexName());
  addUInt64(&argv[argc++], configuration.config.physical_blocks);
  addUInt32(&argv[argc++], 512);
  addUInt32(&argv[argc++], configuration.deviceConfig.cache_size);
  addUInt64(&argv[argc++], configuration.deviceConfig.block_map_maximum_age);
  addString(&argv[argc++], "ack");
  addUInt32(&argv[argc++], 1);
  addString(&argv[argc++], "bio");
  addUInt32(&argv[argc++], DEFAULT_VDO_BIO_SUBMIT_QUEUE_COUNT);
  addString(&argv[argc++], "bioRotationInterval");
  addUInt32(&argv[argc++], DEFAULT_VDO_BIO_SUBMIT_QUEUE_ROTATE_INTERVAL);
  addString(&argv[argc++], "cpu");
  addUInt32(&argv[argc++], 1);
  if (configuration.deviceConfig.thread_counts.hash_zones > 0) {
    addString(&argv[argc++], "hash");
    addUInt32(&argv[argc++],
              configuration.deviceConfig.thread_counts.hash_zones);
  }

  if (configuration.deviceConfig.thread_counts.logical_zones > 0) {
    addString(&argv[argc++], "logical");
    addUInt32(&argv[argc++],
              configuration.deviceConfig.thread_counts.logical_zones);
  }

  if (configuration.deviceConfig.thread_counts.physical_zones > 0) {
    addString(&argv[argc++], "physical");
    addUInt32(&argv[argc++],
              configuration.deviceConfig.thread_counts.physical_zones);
  }

  addString(&argv[argc++], "maxDiscard");
  addUInt32(&argv[argc++], 1500);

  addString(&argv[argc++], "deduplication");
  addString(&argv[argc++],
            (configuration.deviceConfig.deduplication ? "on" : "off"));

  addString(&argv[argc++], "compression");
  addString(&argv[argc++],
            (configuration.deviceConfig.compression ? "on" : "off"));

  addString(&argv[argc++], "compressionType");
  addCompressionType(&argv[argc++], VDO_COMPRESS_LZ4,
                     configuration.deviceConfig.compression_level);
  return argc;
}

/**********************************************************************/
int loadTable(TestConfiguration configuration, struct dm_target *target)
{
  struct dm_dev *dm_dev;
  dm_get_device(NULL, NULL, 0, &dm_dev);
  dm_dev->bdev->size = configuration.config.physical_blocks * VDO_BLOCK_SIZE;

  target->len = configuration.config.logical_blocks * VDO_SECTORS_PER_BLOCK;

  char *argv[32];
  int argc = makeTableLine(fixThreadCounts(configuration), argv);
  int result = vdoTargetType->ctr(target, argc, argv);
  while (argc-- > 0) {
    vdo_free(argv[argc]);
  }

  return result;
}

/**
 * This fake is called from vdo_presuspend to determine the suspend type.
 **/
int dm_noflush_suspending(struct dm_target *ti __attribute__((unused)))
{
  return noFlushSuspend;
}

/**********************************************************************/
int suspendVDO(bool save)
{
  noFlushSuspend = !save;
  vdoTargetType->presuspend(vdo->device_config->owning_target);
  vdoTargetType->postsuspend(vdo->device_config->owning_target);
  return suspend_result;
}

/**********************************************************************/
int resumeVDO(struct dm_target *target)
{
  struct dm_target *old_target = vdo->device_config->owning_target;
  int result = vdoTargetType->preresume(target);
  if (result == VDO_SUCCESS) {
    vdoTargetType->resume(target);
  }

  if (target != old_target) {
    struct dm_target *toDestroy
      = ((vdo->device_config->owning_target == target) ? old_target : target);
      vdoTargetType->dtr(toDestroy);
      vdo_free(toDestroy);
  }

  return resume_result;
}

/**********************************************************************/
int modifyCompressDedupe(bool compress, bool dedupe)
{
  TestConfiguration newConfiguration = configuration;
  newConfiguration.deviceConfig.compression = compress;
  newConfiguration.deviceConfig.deduplication = dedupe;

  struct dm_target *target;
  VDO_ASSERT_SUCCESS(vdo_allocate(1, struct dm_target, __func__, &target));

  int result = loadTable(newConfiguration, target);
  if (result != VDO_SUCCESS) {
    vdo_free(target);
    return result;
  }

  VDO_ASSERT_SUCCESS(suspendVDO(false));

  result = resumeVDO(target);
  if (result == VDO_SUCCESS) {
    configuration.config = vdo->states.vdo.config;
    configuration.deviceConfig.compression = compress;
    configuration.deviceConfig.deduplication = dedupe;
  }

  return result;
}

/**********************************************************************/
static int modifyVDO(block_count_t logicalSize,
		     block_count_t physicalSize,
		     bool          save)
{
  TestConfiguration newConfiguration = configuration;
  newConfiguration.config.physical_blocks = physicalSize;
  newConfiguration.config.logical_blocks = logicalSize;

  struct dm_target *target;
  VDO_ASSERT_SUCCESS(vdo_allocate(1, struct dm_target, __func__, &target));

  int result = loadTable(newConfiguration, target);
  if (result != VDO_SUCCESS) {
    vdo_free(target);
    return result;
  }

  VDO_ASSERT_SUCCESS(suspendVDO(save));
  block_count_t oldSize = synchronousLayer->getBlockCount(synchronousLayer);
  if (oldSize < physicalSize) {
    VDO_ASSERT_SUCCESS(resizeRAMLayer(synchronousLayer, physicalSize));
  }

  result = resumeVDO(target);
  if (result == VDO_SUCCESS) {
    configuration.config = vdo->states.vdo.config;
    configuration.deviceConfig.logical_blocks = logicalSize;
    configuration.deviceConfig.physical_blocks = physicalSize;
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
  block_count_t oldSize = configuration.config.physical_blocks;
  CU_ASSERT_EQUAL(expectedResult,
                  modifyVDO(vdo->device_config->logical_blocks, newSize,
                            false));
  if (expectedResult == VDO_READ_ONLY) {
    verifyReadOnly();
    configuration.config.physical_blocks = oldSize;
  }
}

/**********************************************************************/
void performSuccessfulSuspendAndResume(bool save)
{
  VDO_ASSERT_SUCCESS(suspendVDO(save));
  VDO_ASSERT_SUCCESS(resumeVDO(vdo->device_config->owning_target));
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
