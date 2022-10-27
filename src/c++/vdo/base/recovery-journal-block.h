/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RECOVERY_JOURNAL_BLOCK_H
#define RECOVERY_JOURNAL_BLOCK_H

#include "permassert.h"

#include <linux/bio.h>

#include "packed-recovery-journal-block.h"
#include "recovery-journal.h"
#include "types.h"
#include "wait-queue.h"

struct recovery_journal_block {
	/* The doubly linked pointers for the free or active lists */
	struct list_head list_node;
	/* The waiter for the pending full block list */
	struct waiter write_waiter;
	/* The journal to which this block belongs */
	struct recovery_journal *journal;
	/* A pointer to the current sector in the packed block buffer */
	struct packed_journal_sector *sector;
	/* The vio for writing this block */
	struct vio vio;
	/* The sequence number for this block */
	sequence_number_t sequence_number;
	/* The location of this block in the on-disk journal */
	physical_block_number_t block_number;
	/* Whether this block is being committed */
	bool committing;
	/*
	 * Whether this block has an uncommitted increment for a write with FUA
	 */
	bool has_fua_entry;
	/* The total number of entries in this block */
	journal_entry_count_t entry_count;
	/* The total number of uncommitted entries (queued or committing) */
	journal_entry_count_t uncommitted_entry_count;
	/* The number of new entries in the current commit */
	journal_entry_count_t entries_in_commit;
	/* The queue of vios which will make entries for the next commit */
	struct wait_queue entry_waiters;
	/* The queue of vios waiting for the current commit */
	struct wait_queue commit_waiters;
};

int __must_check
vdo_make_recovery_block(struct vdo *vdo,
			struct recovery_journal *journal,
			struct recovery_journal_block **block_ptr);

void vdo_free_recovery_block(struct recovery_journal_block *block);

void vdo_initialize_recovery_block(struct recovery_journal_block *block);

int __must_check
vdo_enqueue_recovery_block_entry(struct recovery_journal_block *block,
				 struct data_vio *data_vio);

int __must_check vdo_commit_recovery_block(struct recovery_journal_block *block,
					   bio_end_io_t callback,
					   vdo_action *error_handler);

void vdo_dump_recovery_block(const struct recovery_journal_block *block);

bool __must_check
vdo_can_commit_recovery_block(struct recovery_journal_block *block);

#endif /* RECOVERY_JOURNAL_BLOCK_H */
