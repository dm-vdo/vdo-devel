/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_BLOCK_MAP_RECOVERY_H
#define VDO_BLOCK_MAP_RECOVERY_H

#include "block-map.h"
#include "encodings.h"
#include "types.h"

/*
 * An explicitly numbered block mapping. Numbering the mappings allows them to be sorted by logical
 * block number during recovery while still preserving the relative order of journal entries with
 * the same logical block number.
 */
struct numbered_block_mapping {
	struct block_map_slot block_map_slot;
	struct block_map_entry block_map_entry;
	/* A serial number to use during replay */
	u32 number;
} __packed;

void vdo_recover_block_map(struct vdo *vdo,
			   block_count_t entry_count,
			   struct numbered_block_mapping *journal_entries,
			   struct vdo_completion *parent);

#endif /* VDO_BLOCK_MAP_RECOVERY_H */
