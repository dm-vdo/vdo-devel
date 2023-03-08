/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_RECOVERY_H
#define VDO_RECOVERY_H

#include "types.h"

void vdo_replay_into_slab_journals(struct block_allocator *allocator, void *context);
void vdo_repair(struct vdo_completion *parent);

#ifdef INTERNAL
struct numbered_block_mapping;

void recover_block_map(struct vdo *vdo,
		       block_count_t entry_count,
		       struct numbered_block_mapping *journal_entries,
		       struct vdo_completion *parent);
#endif /* INTERNAL */
#endif /* VDO_RECOVERY_H */
