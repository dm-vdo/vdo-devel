/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_REPAIR_H
#define VDO_REPAIR_H

#include "types.h"

void vdo_replay_into_slab_journals(struct block_allocator *allocator,
				   void *context);
void vdo_repair(struct vdo_completion *parent);

#ifdef INTERNAL
struct repair_completion;

void free_repair_completion(struct repair_completion *repair);
void recover_block_map(struct vdo_completion *completion);
#endif /* INTERNAL */
#endif /* VDO_REPAIR_H */
