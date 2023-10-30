/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

/* This file is only used by unit tests. */

#ifndef DM_VDO_TARGET_H
#define DM_VDO_TARGET_H

#include "types.h"

int allocate_instance(unsigned int *instance_ptr);
void release_instance(unsigned int instance);
void initialize_instance_number_tracking(void);
void clean_up_instance_number_tracking(void);
int grow_layout(struct vdo *vdo, block_count_t old_size,
		block_count_t new_size);

#endif /* DM_VDO_TARGET_H */
