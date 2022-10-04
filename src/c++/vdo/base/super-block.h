/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef SUPER_BLOCK_H
#define SUPER_BLOCK_H

#include "completion.h"
#include "kernel-types.h"
#include "types.h"

struct vdo_super_block;

void vdo_free_super_block(struct vdo_super_block *super_block);

void vdo_save_super_block(struct vdo_super_block *super_block,
			  physical_block_number_t super_block_offset,
			  struct vdo_completion *parent);

void vdo_load_super_block(struct vdo *vdo,
			  struct vdo_completion *parent,
			  physical_block_number_t super_block_offset,
			  struct vdo_super_block **super_block_ptr);

struct super_block_codec * __must_check
vdo_get_super_block_codec(struct vdo_super_block *super_block);

#ifdef INTERNAL
int __must_check vdo_make_super_block(struct vdo *vdo,
				      struct vdo_super_block **super_block_ptr);
#endif /* INTERNAL */
#endif /* SUPER_BLOCK_H */
