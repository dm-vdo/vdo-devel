/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VIO_READ_H
#define VIO_READ_H

#include "kernel-types.h"

void continue_read_with_block_map_slot(struct vdo_completion *completion);
void cleanup_read_data_vio(struct data_vio *data_vio);

#endif /* VIO_READ_H */
