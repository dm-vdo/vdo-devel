/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef ADMIN_UTILS_H
#define ADMIN_UTILS_H

#include "admin-state.h"
#include "completion.h"
#include "packer.h"
#include "types.h"

/**
 * Launch an operation on a slab and return the completion. The caller is
 * responsible for waiting on the completion and freeing it.
 *
 * @param slab       The slab to act upon
 * @param operation  The operation to perform on the slab
 *
 * @return The action completion
 **/
struct vdo_completion *
launchSlabAction(struct vdo_slab               *slab,
                 const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a slab and return the result.
 *
 * @param slab       The slab to act upon
 * @param operation  The operation to perform on the slab
 *
 * @return The result of the action
 **/
int performSlabAction(struct vdo_slab               *slab,
                      const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a slab and assert that it succeeded.
 *
 * @param slab       The slab to act upon
 * @param operation  The operation to perform on the slab
 **/
void performSuccessfulSlabAction(struct vdo_slab               *slab,
                                 const struct admin_state_code *operation);

/**
 * Launch an operation on a depot and return the completion. The caller is
 * responsible for waiting on the completion and freeing it.
 *
 * @param depot      The depot to act upon
 * @param operation  The operation to perform on the depot
 *
 * @return The action completion
 **/
struct vdo_completion *
launchDepotAction(struct slab_depot             *depot,
                  const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a depot and return the result.
 *
 * @param depot      The depot to act upon
 * @param operation  The operation to perform on the depot
 *
 * @return The result of the action
 **/
int performDepotAction(struct slab_depot             *depot,
                       const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a depot.
 *
 * @param depot      The depot to act upon
 * @param operation  The operation to perform on the depot
 **/
void
performSuccessfulDepotActionOnDepot(struct slab_depot             *depot,
                                    const struct admin_state_code *operation);

/**
 * Perform an operation on the slab depot from the test's VDO.
 *
 * @param operation  The operation to perform on the depot
 **/
void performSuccessfulDepotAction(const struct admin_state_code *operation);

/**
 * Launch an operation on a block map and return the completion. The caller is
 * responsible for waiting on the completion and freeing it.
 *
 * @param map        The block map to act upon
 * @param operation  The operation to perform on the block map
 *
 * @return The action completion
 **/
struct vdo_completion *
launchBlockMapAction(struct block_map              *map,
                     const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a block map and return the result.
 *
 * @param map        The block map to act upon
 * @param operation  The operation to perform on the block map
 *
 * @return The result of the action
 **/
int performBlockMapAction(struct block_map              *map,
                          const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a block map and assert that it succeeded.
 *
 * @param map        The block map to act upon
 * @param operation  The operation to perform on the block map
 **/
void
performSuccessfulBlockMapActionOnMap(struct block_map              *map,
                                     const struct admin_state_code *operation);

/**
 * Perform an operation on the block map from the test's VDO.
 *
 * @param operation  The operation to perform on the block map
 **/
void performSuccessfulBlockMapAction(const struct admin_state_code *operation);

/**
 * Launch an operation on a packer and return the completion. The caller is
 * responsible for waiting on the completion and freeing it.
 *
 * @param packer     The packer to act upon
 * @param operation  The operation to perform on the packer
 *
 * @return The action completion
 **/
struct vdo_completion *
launchPackerAction(struct packer                 *packer,
                   const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a packer and return the result.
 *
 * @param packer     The packer to act upon
 * @param operation  The operation to perform on the packer
 *
 * @return The result of the action
 **/
int performPackerAction(struct packer                 *packer,
                        const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a packer and assert that it succeeded.
 *
 * @param packer     The packer to act upon
 * @param operation  The operation to perform on the packer
 **/
void
performSuccessfulPackerActionOnPacker(struct packer                 *packer,
                                      const struct admin_state_code *operation);

/**
 * Perform an operation on the packer from the test's VDO.
 *
 * @param operation  The operation to perform on the packer
 **/
void performSuccessfulPackerAction(const struct admin_state_code *operation);

/**
 * Launch an operation on a recovery journal and return the completion. The
 * caller is responsible for waiting on the completion and freeing it.
 *
 * @param journal    The recovery journal to act upon
 * @param operation  The operation to perform on the recovery journal
 *
 * @return The action completion
 **/
struct vdo_completion *
launchRecoveryJournalAction(struct recovery_journal       *journal,
                            const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a recovery journal and return the result.
 *
 * @param journal    The recovery journal to act upon
 * @param operation  The operation to perform on the recovery journal
 *
 * @return The result of the action
 **/
int performRecoveryJournalAction(struct recovery_journal       *journal,
                                 const struct admin_state_code *operation)
  __attribute__((warn_unused_result));

/**
 * Perform an operation on a recovery journal and assert that it succeeded.
 *
 * @param journal    The recovery journal to act upon
 * @param operation  The operation to perform on the recovery journal
 **/
void
performSuccessfulRecoveryJournalActionOnJournal(struct recovery_journal *journal,
                                                const struct admin_state_code *operation);

/**
 * Perform an operation on the recovery journal from the test's VDO.
 *
 * @param operation  The operation to perform on the recovery journal
 **/
void
performSuccessfulRecoveryJournalAction(const struct admin_state_code *operation);

#endif // ADMIN_UTILS_H
