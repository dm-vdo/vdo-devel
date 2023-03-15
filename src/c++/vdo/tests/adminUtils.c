/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "adminUtils.h"

#include "memory-alloc.h"

#include "admin-state.h"
#include "block-map.h"
#include "completion.h"
#include "packer.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "types.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  struct vdo_completion          completion;
  void                          *operand;
  const struct admin_state_code *operation;
} AdminOperationCompletion;

/**********************************************************************/
static struct vdo_completion *
launchAdminAction(void                          *operand,
                  const struct admin_state_code *operation,
                  vdo_action                    *action,
                  thread_id_t                    threadID)
{
  AdminOperationCompletion *adminOperation;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, AdminOperationCompletion, __func__,
                                  &adminOperation));
  *adminOperation = (AdminOperationCompletion) {
    .operand      = operand,
    .operation    = operation,
  };

  struct vdo_completion *completion = &adminOperation->completion;
  vdo_initialize_completion(completion, vdo, VDO_TEST_COMPLETION);
  completion->callback_thread_id    = threadID;
  launchAction(action, completion);
  return completion;
}

/**********************************************************************/
static void slabOperationAction(struct vdo_completion *completion)
{
  AdminOperationCompletion *adminOperation = (AdminOperationCompletion *) completion;
  struct vdo_slab               *slab      = adminOperation->operand;
  const struct admin_state_code *operation = adminOperation->operation;
  list_del_init(&slab->allocq_entry);
  vdo_start_operation_with_waiter(&slab->state, operation, completion, initiate_slab_action);
}

/**********************************************************************/
struct vdo_completion *
launchSlabAction(struct vdo_slab               *slab,
                 const struct admin_state_code *operation)
{
  return launchAdminAction(slab, operation, slabOperationAction,
                           slab->allocator->thread_id);
}

/**********************************************************************/
int performSlabAction(struct vdo_slab               *slab,
                      const struct admin_state_code *operation)
{
  struct vdo_completion *completion = launchSlabAction(slab, operation);
  int                    result     = awaitCompletion(completion);
  UDS_FREE(completion);
  return result;
}

/**********************************************************************/
void performSuccessfulSlabAction(struct vdo_slab               *slab,
                                 const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performSlabAction(slab, operation));
}

/**********************************************************************/
static void slabDepotOperationAction(struct vdo_completion *completion)
{
  AdminOperationCompletion *adminOperation
    = (AdminOperationCompletion *) completion;
  struct slab_depot    *depot     = adminOperation->operand;
  const struct admin_state_code *operation = adminOperation->operation;
  if (operation->draining) {
    vdo_drain_slab_depot(depot, operation, completion);
  } else if (operation->loading) {
    vdo_load_slab_depot(depot, operation, completion, NULL);
  } else if (operation == VDO_ADMIN_STATE_RESUMING) {
    vdo_resume_slab_depot(depot, completion);
  } else {
    CU_FAIL("Invalid slab depot operation %s",
            adminOperation->operation->name);
  }
}

/**********************************************************************/
struct vdo_completion *
launchDepotAction(struct slab_depot             *depot,
                  const struct admin_state_code *operation)
{
  return launchAdminAction(depot, operation, slabDepotOperationAction, 0);
}

/**********************************************************************/
int performDepotAction(struct slab_depot             *depot,
                       const struct admin_state_code *operation)
{
  struct vdo_completion *completion = launchDepotAction(depot, operation);
  int                    result     = awaitCompletion(completion);
  UDS_FREE(completion);
  return result;
}

/**********************************************************************/
void
performSuccessfulDepotActionOnDepot(struct slab_depot             *depot,
                                    const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performDepotAction(depot, operation));
}

/**********************************************************************/
void performSuccessfulDepotAction(const struct admin_state_code *operation)
{
  performSuccessfulDepotActionOnDepot(vdo->depot, operation);
}

/**********************************************************************/
static void blockMapOperationAction(struct vdo_completion *completion)
{
  AdminOperationCompletion *adminOperation
    = (AdminOperationCompletion *) completion;
  struct block_map              *map = adminOperation->operand;
  const struct admin_state_code *operation    = adminOperation->operation;
  if (operation->draining) {
    vdo_drain_block_map(map, operation, completion);
  } else if (operation == VDO_ADMIN_STATE_RESUMING) {
    vdo_resume_block_map(map, completion);
  } else {
    CU_FAIL("Invalid block map operation %s",
            adminOperation->operation->name);
  }
}

/**********************************************************************/
struct vdo_completion *
launchBlockMapAction(struct block_map              *map,
                     const struct admin_state_code *operation)
{
  return launchAdminAction(map, operation, blockMapOperationAction,
                           vdo->thread_config->admin_thread);
}

/**********************************************************************/
int performBlockMapAction(struct block_map              *map,
                          const struct admin_state_code *operation)
{
  struct vdo_completion *completion = launchBlockMapAction(map, operation);
  int                    result     = awaitCompletion(completion);
  UDS_FREE(completion);
  return result;
}

/**********************************************************************/
void
performSuccessfulBlockMapActionOnMap(struct block_map              *map,
                                     const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performBlockMapAction(map, operation));
}

/**********************************************************************/
void performSuccessfulBlockMapAction(const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performBlockMapAction(vdo->block_map, operation));
}

/**********************************************************************/
static void packerOperationAction(struct vdo_completion *completion)
{
  AdminOperationCompletion *adminOperation
    = (AdminOperationCompletion *) completion;
  struct packer *packer           = adminOperation->operand;
  const struct admin_state_code *operation = adminOperation->operation;
  if (operation->draining) {
    vdo_drain_packer(packer, completion);
  } else if (operation == VDO_ADMIN_STATE_RESUMING) {
    vdo_resume_packer(packer, completion);
  } else {
    CU_FAIL("Invalid packer operation %s",
            adminOperation->operation->name);
  }
}

/**********************************************************************/
struct vdo_completion *
launchPackerAction(struct packer                 *packer,
                   const struct admin_state_code *operation)
{
  return launchAdminAction(packer, operation, packerOperationAction,
                           vdo->thread_config->packer_thread);
}

/**********************************************************************/
int performPackerAction(struct packer                 *packer,
                        const struct admin_state_code *operation)
{
  struct vdo_completion *completion = launchPackerAction(packer, operation);
  int result = awaitCompletion(completion);
  UDS_FREE(completion);
  return result;
}

/**********************************************************************/
void
performSuccessfulPackerActionOnPacker(struct packer                 *packer,
                                      const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performPackerAction(packer, operation));
}

/**********************************************************************/
void performSuccessfulPackerAction(const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performPackerAction(vdo->packer, operation));
}

/**********************************************************************/
static void recoveryJournalOperationAction(struct vdo_completion *completion)
{
  AdminOperationCompletion *adminOperation
    = (AdminOperationCompletion *) completion;
  struct recovery_journal       *journal   = adminOperation->operand;
  const struct admin_state_code *operation = adminOperation->operation;
  if (operation->draining) {
    vdo_drain_recovery_journal(journal, operation, completion);
  } else if (operation == VDO_ADMIN_STATE_RESUMING) {
    vdo_resume_recovery_journal(journal, completion);
  } else {
    CU_FAIL("Invalid recovery journal operation %s",
            adminOperation->operation->name);
  }
}

/**********************************************************************/
struct vdo_completion *
launchRecoveryJournalAction(struct recovery_journal       *journal,
                            const struct admin_state_code *operation)
{
  return launchAdminAction(journal, operation, recoveryJournalOperationAction,
                           journal->thread_id);
}

/**********************************************************************/
int performRecoveryJournalAction(struct recovery_journal       *journal,
                                 const struct admin_state_code *operation)
{
  struct vdo_completion *completion
    = launchRecoveryJournalAction(journal, operation);
  int result = awaitCompletion(completion);
  UDS_FREE(completion);
  return result;
}

/**********************************************************************/
void
performSuccessfulRecoveryJournalActionOnJournal(struct recovery_journal       *journal,
                                                const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performRecoveryJournalAction(journal, operation));
}

/**********************************************************************/
void
performSuccessfulRecoveryJournalAction(const struct admin_state_code *operation)
{
  VDO_ASSERT_SUCCESS(performRecoveryJournalAction(vdo->recovery_journal,
                                                  operation));
}
