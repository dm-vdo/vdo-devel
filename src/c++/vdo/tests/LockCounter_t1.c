/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "recovery-journal.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  LOGICAL_ZONES          = 2,
  PHYSICAL_ZONES         = 3,
  HASH_ZONES             = 2,
  LOGICAL_THREAD_COUNT   = LOGICAL_ZONES,
  PHYSICAL_THREAD_COUNT  = PHYSICAL_ZONES,
  HASH_ZONE_THREAD_COUNT = HASH_ZONES,
  LOCKS                  = 4,
  BATCH_SIZE             = 10,
  LOCK_NUMBER            = 1,
};

typedef struct {
  struct vdo_completion completion;
  enum vdo_zone_type    zoneType;
  zone_count_t          zoneID;
  int32_t               adjustment;
} LockClient;

static int         notificationCount = 0;
static vdo_action *counterCallback;

/**
 * Implements LockedMethod.
 **/
static bool signalNotification(void *context __attribute__((unused)))
{
  notificationCount++;
  return true;
}

/**
 * Count the number of times the lock counter goes from one to zero and
 * acknowledge the unlocking.
 **/
static void
countNotification(struct vdo_completion *completion __attribute__((unused)))
{
  counterCallback(completion);
  runLocked(signalNotification, NULL);
}

/**
 * Implements WaitCondition.
 **/
static bool checkNotificationCount(void *context)
{
  return (notificationCount >= *((int *) context));
}

/**
 * Implements WaitCondition.
 **/
static bool checkExactNotificationCount(void *context)
{
  CU_ASSERT_TRUE(notificationCount <= *((int *) context));
  return checkNotificationCount(context);
}

/**
 * Test specific setup.
 **/
static void initializeLockCounter_t1(void)
{
  const TestParameters parameters = {
    .logicalThreadCount  = LOGICAL_THREAD_COUNT,
    .physicalThreadCount = PHYSICAL_THREAD_COUNT,
    .hashZoneThreadCount = HASH_ZONE_THREAD_COUNT,
    .journalBlocks       = LOCKS,
  };

  initializeVDOTest(&parameters);
  notificationCount = 0;
  counterCallback = vdo->recovery_journal->lock_counter.completion.callback;
  vdo->recovery_journal->lock_counter.completion.callback = countNotification;
}

/**
 * Cast a vdo_completion as a lock client.
 **/
static LockClient *completionAsClient(struct vdo_completion *completion)
{
  vdo_assert_completion_type(completion, VDO_TEST_COMPLETION);
  return container_of(completion, LockClient, completion);
}

/**
 * A VDOAction that adjusts the reference count.
 **/
static void doAdjustment(struct vdo_completion *completion)
{
  LockClient *client = completionAsClient(completion);
  struct recovery_journal *journal = vdo->recovery_journal;
  if (client->adjustment == -1) {
    vdo_release_recovery_journal_block_reference(journal,
                                                 LOCK_NUMBER,
                                                 client->zoneType,
                                                 client->zoneID);
  } else if (client->zoneType == VDO_ZONE_TYPE_JOURNAL) {
    struct lock_counter *counter = &journal->lock_counter;
    counter->journal_counters[LOCK_NUMBER] = client->adjustment;
    atomic_set(&(counter->journal_decrement_counts[LOCK_NUMBER]), 0);
  } else if (client->adjustment == 1) {
    vdo_acquire_recovery_journal_block_reference(journal,
                                                 LOCK_NUMBER,
                                                 client->zoneType,
                                                 client->zoneID);
  } else {
    CU_FAIL("Non-journal zone adjustment is not of magnitude 1");
  }

  vdo_finish_completion(completion);
}

/**
 * Launch a VDOAction that adjusts the reference count.
 **/
static struct vdo_completion *launchAdjustment(enum vdo_zone_type zoneType,
                                               zone_count_t       zoneID,
                                               int32_t            adjustment)
{
  LockClient *client;
  VDO_ASSERT_SUCCESS(uds_allocate(1, LockClient, __func__, &client));
  vdo_initialize_completion(&client->completion, vdo, VDO_TEST_COMPLETION);
  client->completion.callback_thread_id = zoneID; // Use zone ID as thread ID.
  client->zoneType                      = zoneType;
  client->zoneID                        = zoneID;
  client->adjustment                    = adjustment;
  launchAction(doAdjustment, &client->completion);
  return &client->completion;
}

/**
 * Wait for the action of adjusting reference count to finish.
 **/
static void waitForAdjustmentFinished(struct vdo_completion *completion)
{
  VDO_ASSERT_SUCCESS(awaitCompletion(completion));
  LockClient *client = completionAsClient(completion);
  free(client);
}

/**
 * Perform a reference count adjustment.
 **/
static void performAdjustment(enum vdo_zone_type zoneType,
                              zone_count_t       zoneID,
                              int32_t            adjustment)
{
  waitForAdjustmentFinished(launchAdjustment(zoneType, zoneID, adjustment));
}

/**
 * Test that locks can be acquired and released from within the same zone type
 * correctly.
 **/
static void sameZoneTypeTest(void)
{
  struct recovery_journal *journal = vdo->recovery_journal;

  for (int iteration = 1; iteration <= 3; iteration++) {
    struct vdo_completion *zone0[BATCH_SIZE];
    struct vdo_completion *zone1[BATCH_SIZE];
    for (int i = 0; i < BATCH_SIZE; i++) {
      zone0[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, 1);
      zone1[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 1, 1);
    }

    for (int i = 0; i < BATCH_SIZE; i++) {
      waitForAdjustmentFinished(zone0[i]);
      waitForAdjustmentFinished(zone1[i]);
    }

    CU_ASSERT_TRUE(is_lock_locked(journal,
                                  LOCK_NUMBER,
                                  VDO_ZONE_TYPE_LOGICAL));
    CU_ASSERT_FALSE(is_lock_locked(journal,
                                   LOCK_NUMBER,
                                   VDO_ZONE_TYPE_PHYSICAL));

    for (int i = 0; i < BATCH_SIZE; i++) {
      zone0[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, -1);
      zone1[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 1, -1);
    }

    for (int i = 0; i < BATCH_SIZE; i++) {
      waitForAdjustmentFinished(zone0[i]);
      waitForAdjustmentFinished(zone1[i]);
    }

    waitForCondition(checkNotificationCount, &iteration);
    CU_ASSERT_FALSE(is_lock_locked(journal,
                                   LOCK_NUMBER,
                                   VDO_ZONE_TYPE_LOGICAL));
  }
}

/**
 * Test that locks can be acquire and release from different zone types
 * correctly.
 **/
static void differentZoneTypeTest(void)
{
  struct recovery_journal *journal = vdo->recovery_journal;

  for (int iteration = 1; iteration <= 3; iteration++) {
    // Initialize the locks in the journal zone
    performAdjustment(VDO_ZONE_TYPE_JOURNAL, 0, BATCH_SIZE * 2);

    // Journal zone already locks a block number so the ordering between
    // logical and physical zone adjustment don't matter.
    struct vdo_completion *logical[BATCH_SIZE * 2];
    struct vdo_completion *physical[BATCH_SIZE * 2];
    for (int i = 0; i < BATCH_SIZE; i++) {
      logical[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, 1);
      physical[i] = launchAdjustment(VDO_ZONE_TYPE_PHYSICAL, 0, 1);
    }

    CU_ASSERT_TRUE(is_lock_locked(journal,
                                  LOCK_NUMBER,
                                  VDO_ZONE_TYPE_LOGICAL));
    CU_ASSERT_TRUE(is_lock_locked(journal,
                                  LOCK_NUMBER,
                                  VDO_ZONE_TYPE_PHYSICAL));

    for (int i = BATCH_SIZE; i < BATCH_SIZE * 2; i++) {
      logical[i] = launchAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, -1);
      physical[i] = launchAdjustment(VDO_ZONE_TYPE_PHYSICAL, 0, -1);
    }

    for (int i = 0; i < BATCH_SIZE * 2; i++) {
      waitForAdjustmentFinished(logical[i]);
      waitForAdjustmentFinished(physical[i]);
    }

    struct vdo_completion *journalCompletions[BATCH_SIZE];
    for (int i = 0; i < BATCH_SIZE; i++) {
      vdo_release_journal_entry_lock(journal, LOCK_NUMBER);
      journalCompletions[i] = launchAdjustment(VDO_ZONE_TYPE_JOURNAL, 0, -1);
    }

    for (int i = 0; i < BATCH_SIZE; i++) {
      waitForAdjustmentFinished(journalCompletions[i]);
    }

    waitForCondition(checkNotificationCount, &iteration);
    CU_ASSERT_FALSE(is_lock_locked(journal,
                                   LOCK_NUMBER,
                                   VDO_ZONE_TYPE_LOGICAL));
    CU_ASSERT_FALSE(is_lock_locked(journal,
                                   LOCK_NUMBER,
                                   VDO_ZONE_TYPE_PHYSICAL));
  }
}

/**
 * Test that each zone type will send a notification when it unlocks.
 **/
static void testNotification(void)
{
  performAdjustment(VDO_ZONE_TYPE_JOURNAL, 0, 2);
  performAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, 1);
  performAdjustment(VDO_ZONE_TYPE_LOGICAL, 1, 1);
  performAdjustment(VDO_ZONE_TYPE_PHYSICAL, 0, 1);
  performAdjustment(VDO_ZONE_TYPE_PHYSICAL, 1, 1);

  performAdjustment(VDO_ZONE_TYPE_JOURNAL, 0, -1);
  performAdjustment(VDO_ZONE_TYPE_LOGICAL, 0, -1);
  performAdjustment(VDO_ZONE_TYPE_PHYSICAL, 0, -1);

  int expectedCount = 1;
  performAdjustment(VDO_ZONE_TYPE_JOURNAL, 0, -1);
  waitForCondition(checkExactNotificationCount, &expectedCount);

  expectedCount++;
  performAdjustment(VDO_ZONE_TYPE_LOGICAL, 1, -1);
  waitForCondition(checkExactNotificationCount, &expectedCount);

  expectedCount++;
  performAdjustment(VDO_ZONE_TYPE_PHYSICAL, 1, -1);
  waitForCondition(checkExactNotificationCount, &expectedCount);
}

/**********************************************************************/
static CU_TestInfo lockCounterTests[] = {
  { "within same zone type", sameZoneTypeTest      },
  { "different zone type",   differentZoneTypeTest },
  { "notifications",         testNotification      },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo lockCounterSuite = {
  .name                     = "Lock counters (LockCounter_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeLockCounter_t1,
  .cleaner                  = tearDownVDOTest,
  .tests                    = lockCounterTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &lockCounterSuite;
}
