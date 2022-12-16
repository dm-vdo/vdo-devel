/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_RECOVERY_H
#define VDO_RECOVERY_H

#include "completion.h"
#include "vdo.h"

void vdo_replay_into_slab_journals(struct block_allocator *allocator, void *context);
void vdo_repair(struct vdo_completion *parent);

#endif /* VDO_RECOVERY_H */
