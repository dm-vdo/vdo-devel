// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/mutex.h>

#include "logger.h"
#include "string-utils.h"

#include "dedupe.h"
#include "pool-sysfs.h"
#include "statistics.h"
#include "vdo.h"

struct pool_stats_attribute {
	struct attribute attr;
	ssize_t (*print)(struct vdo_statistics *stats, char *buf);
};

static ssize_t pool_stats_attr_show(struct kobject *directory, struct attribute *attr,
				    char *buf)
{
	ssize_t size;
	struct pool_stats_attribute *pool_stats_attr =
		container_of(attr, struct pool_stats_attribute, attr);
	struct vdo *vdo = container_of(directory, struct vdo, stats_directory);

	if (pool_stats_attr->print == NULL)
		return -EINVAL;

	mutex_lock(&vdo->stats_mutex);
	vdo_fetch_statistics(vdo, &vdo->stats_buffer);
	size = pool_stats_attr->print(&vdo->stats_buffer, buf);
	mutex_unlock(&vdo->stats_mutex);

	return size;
}

const struct sysfs_ops vdo_pool_stats_sysfs_ops = {
	.show = pool_stats_attr_show,
	.store = NULL,
};

/* Number of blocks used for data */
static ssize_t pool_stats_print_data_blocks_used(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->data_blocks_used);
	#else
	return sprintf(buf, "%lu\n", stats->data_blocks_used);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_data_blocks_used = {
	.attr = { .name = "data_blocks_used", .mode = 0444, },
	.print = pool_stats_print_data_blocks_used,
};

/* Number of blocks used for VDO metadata */
static ssize_t pool_stats_print_overhead_blocks_used(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->overhead_blocks_used);
	#else
	return sprintf(buf, "%lu\n", stats->overhead_blocks_used);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_overhead_blocks_used = {
	.attr = { .name = "overhead_blocks_used", .mode = 0444, },
	.print = pool_stats_print_overhead_blocks_used,
};

/* Number of logical blocks that are currently mapped to physical blocks */
static ssize_t pool_stats_print_logical_blocks_used(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->logical_blocks_used);
	#else
	return sprintf(buf, "%lu\n", stats->logical_blocks_used);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_logical_blocks_used = {
	.attr = { .name = "logical_blocks_used", .mode = 0444, },
	.print = pool_stats_print_logical_blocks_used,
};

/* number of physical blocks */
static ssize_t pool_stats_print_physical_blocks(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->physical_blocks);
	#else
	return sprintf(buf, "%lu\n", stats->physical_blocks);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_physical_blocks = {
	.attr = { .name = "physical_blocks", .mode = 0444, },
	.print = pool_stats_print_physical_blocks,
};

/* number of logical blocks */
static ssize_t pool_stats_print_logical_blocks(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->logical_blocks);
	#else
	return sprintf(buf, "%lu\n", stats->logical_blocks);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_logical_blocks = {
	.attr = { .name = "logical_blocks", .mode = 0444, },
	.print = pool_stats_print_logical_blocks,
};

/* Size of the block map page cache, in bytes */
static ssize_t pool_stats_print_block_map_cache_size(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map_cache_size);
	#else
	return sprintf(buf, "%lu\n", stats->block_map_cache_size);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_cache_size = {
	.attr = { .name = "block_map_cache_size", .mode = 0444, },
	.print = pool_stats_print_block_map_cache_size,
};

/* The physical block size */
static ssize_t pool_stats_print_block_size(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_size);
	#else
	return sprintf(buf, "%lu\n", stats->block_size);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_size = {
	.attr = { .name = "block_size", .mode = 0444, },
	.print = pool_stats_print_block_size,
};

/* Number of times the VDO has successfully recovered */
static ssize_t pool_stats_print_complete_recoveries(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->complete_recoveries);
	#else
	return sprintf(buf, "%lu\n", stats->complete_recoveries);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_complete_recoveries = {
	.attr = { .name = "complete_recoveries", .mode = 0444, },
	.print = pool_stats_print_complete_recoveries,
};

/* Number of times the VDO has recovered from read-only mode */
static ssize_t pool_stats_print_read_only_recoveries(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->read_only_recoveries);
	#else
	return sprintf(buf, "%lu\n", stats->read_only_recoveries);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_read_only_recoveries = {
	.attr = { .name = "read_only_recoveries", .mode = 0444, },
	.print = pool_stats_print_read_only_recoveries,
};

/* String describing the operating mode of the VDO */
static ssize_t pool_stats_print_mode(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%s\n", stats->mode);
	#else
	return sprintf(buf, "%s\n", stats->mode);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_mode = {
	.attr = { .name = "mode", .mode = 0444, },
	.print = pool_stats_print_mode,
};

/* Whether the VDO is in recovery mode */
static ssize_t pool_stats_print_in_recovery_mode(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%d\n", stats->in_recovery_mode);
	#else
	return sprintf(buf, "%d\n", stats->in_recovery_mode);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_in_recovery_mode = {
	.attr = { .name = "in_recovery_mode", .mode = 0444, },
	.print = pool_stats_print_in_recovery_mode,
};

/* What percentage of recovery mode work has been completed */
static ssize_t pool_stats_print_recovery_percentage(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->recovery_percentage);
	#else
	return sprintf(buf, "%hhu\n", stats->recovery_percentage);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_recovery_percentage = {
	.attr = { .name = "recovery_percentage", .mode = 0444, },
	.print = pool_stats_print_recovery_percentage,
};

/* Number of compressed data items written since startup */
static ssize_t pool_stats_print_packer_compressed_fragments_written(struct vdo_statistics *stats,
								    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->packer.compressed_fragments_written);
	#else
	return sprintf(buf, "%lu\n", stats->packer.compressed_fragments_written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_packer_compressed_fragments_written = {
	.attr = { .name = "packer_compressed_fragments_written", .mode = 0444, },
	.print = pool_stats_print_packer_compressed_fragments_written,
};

/* Number of blocks containing compressed items written since startup */
static ssize_t pool_stats_print_packer_compressed_blocks_written(struct vdo_statistics *stats,
								 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->packer.compressed_blocks_written);
	#else
	return sprintf(buf, "%lu\n", stats->packer.compressed_blocks_written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_packer_compressed_blocks_written = {
	.attr = { .name = "packer_compressed_blocks_written", .mode = 0444, },
	.print = pool_stats_print_packer_compressed_blocks_written,
};

/* Number of VIOs that are pending in the packer */
static ssize_t pool_stats_print_packer_compressed_fragments_in_packer(struct vdo_statistics *stats,
								      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->packer.compressed_fragments_in_packer);
	#else
	return sprintf(buf, "%lu\n", stats->packer.compressed_fragments_in_packer);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_packer_compressed_fragments_in_packer = {
	.attr = { .name = "packer_compressed_fragments_in_packer", .mode = 0444, },
	.print = pool_stats_print_packer_compressed_fragments_in_packer,
};

/* The total number of slabs from which blocks may be allocated */
static ssize_t pool_stats_print_allocator_slab_count(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->allocator.slab_count);
	#else
	return sprintf(buf, "%lu\n", stats->allocator.slab_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_allocator_slab_count = {
	.attr = { .name = "allocator_slab_count", .mode = 0444, },
	.print = pool_stats_print_allocator_slab_count,
};

/* The total number of slabs from which blocks have ever been allocated */
static ssize_t pool_stats_print_allocator_slabs_opened(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->allocator.slabs_opened);
	#else
	return sprintf(buf, "%lu\n", stats->allocator.slabs_opened);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_allocator_slabs_opened = {
	.attr = { .name = "allocator_slabs_opened", .mode = 0444, },
	.print = pool_stats_print_allocator_slabs_opened,
};

/* The number of times since loading that a slab has been re-opened */
static ssize_t pool_stats_print_allocator_slabs_reopened(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->allocator.slabs_reopened);
	#else
	return sprintf(buf, "%lu\n", stats->allocator.slabs_reopened);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_allocator_slabs_reopened = {
	.attr = { .name = "allocator_slabs_reopened", .mode = 0444, },
	.print = pool_stats_print_allocator_slabs_reopened,
};

/* Number of times the on-disk journal was full */
static ssize_t pool_stats_print_journal_disk_full(struct vdo_statistics *stats,
						  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.disk_full);
	#else
	return sprintf(buf, "%lu\n", stats->journal.disk_full);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_disk_full = {
	.attr = { .name = "journal_disk_full", .mode = 0444, },
	.print = pool_stats_print_journal_disk_full,
};

/* Number of times the recovery journal requested slab journal commits. */
static ssize_t pool_stats_print_journal_slab_journal_commits_requested(struct vdo_statistics *stats,
								       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.slab_journal_commits_requested);
	#else
	return sprintf(buf, "%lu\n", stats->journal.slab_journal_commits_requested);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_slab_journal_commits_requested = {
	.attr = { .name = "journal_slab_journal_commits_requested", .mode = 0444, },
	.print = pool_stats_print_journal_slab_journal_commits_requested,
};

/* The total number of items on which processing has started */
static ssize_t pool_stats_print_journal_entries_started(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.entries.started);
	#else
	return sprintf(buf, "%lu\n", stats->journal.entries.started);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_entries_started = {
	.attr = { .name = "journal_entries_started", .mode = 0444, },
	.print = pool_stats_print_journal_entries_started,
};

/* The total number of items for which a write operation has been issued */
static ssize_t pool_stats_print_journal_entries_written(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.entries.written);
	#else
	return sprintf(buf, "%lu\n", stats->journal.entries.written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_entries_written = {
	.attr = { .name = "journal_entries_written", .mode = 0444, },
	.print = pool_stats_print_journal_entries_written,
};

/* The total number of items for which a write operation has completed */
static ssize_t pool_stats_print_journal_entries_committed(struct vdo_statistics *stats,
							  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.entries.committed);
	#else
	return sprintf(buf, "%lu\n", stats->journal.entries.committed);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_entries_committed = {
	.attr = { .name = "journal_entries_committed", .mode = 0444, },
	.print = pool_stats_print_journal_entries_committed,
};

/* The total number of items on which processing has started */
static ssize_t pool_stats_print_journal_blocks_started(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.blocks.started);
	#else
	return sprintf(buf, "%lu\n", stats->journal.blocks.started);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_blocks_started = {
	.attr = { .name = "journal_blocks_started", .mode = 0444, },
	.print = pool_stats_print_journal_blocks_started,
};

/* The total number of items for which a write operation has been issued */
static ssize_t pool_stats_print_journal_blocks_written(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.blocks.written);
	#else
	return sprintf(buf, "%lu\n", stats->journal.blocks.written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_blocks_written = {
	.attr = { .name = "journal_blocks_written", .mode = 0444, },
	.print = pool_stats_print_journal_blocks_written,
};

/* The total number of items for which a write operation has completed */
static ssize_t pool_stats_print_journal_blocks_committed(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->journal.blocks.committed);
	#else
	return sprintf(buf, "%lu\n", stats->journal.blocks.committed);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_journal_blocks_committed = {
	.attr = { .name = "journal_blocks_committed", .mode = 0444, },
	.print = pool_stats_print_journal_blocks_committed,
};

/* Number of times the on-disk journal was full */
static ssize_t pool_stats_print_slab_journal_disk_full_count(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_journal.disk_full_count);
	#else
	return sprintf(buf, "%lu\n", stats->slab_journal.disk_full_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_journal_disk_full_count = {
	.attr = { .name = "slab_journal_disk_full_count", .mode = 0444, },
	.print = pool_stats_print_slab_journal_disk_full_count,
};

/* Number of times an entry was added over the flush threshold */
static ssize_t pool_stats_print_slab_journal_flush_count(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_journal.flush_count);
	#else
	return sprintf(buf, "%lu\n", stats->slab_journal.flush_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_journal_flush_count = {
	.attr = { .name = "slab_journal_flush_count", .mode = 0444, },
	.print = pool_stats_print_slab_journal_flush_count,
};

/* Number of times an entry was added over the block threshold */
static ssize_t pool_stats_print_slab_journal_blocked_count(struct vdo_statistics *stats,
							   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_journal.blocked_count);
	#else
	return sprintf(buf, "%lu\n", stats->slab_journal.blocked_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_journal_blocked_count = {
	.attr = { .name = "slab_journal_blocked_count", .mode = 0444, },
	.print = pool_stats_print_slab_journal_blocked_count,
};

/* Number of times a tail block was written */
static ssize_t pool_stats_print_slab_journal_blocks_written(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_journal.blocks_written);
	#else
	return sprintf(buf, "%lu\n", stats->slab_journal.blocks_written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_journal_blocks_written = {
	.attr = { .name = "slab_journal_blocks_written", .mode = 0444, },
	.print = pool_stats_print_slab_journal_blocks_written,
};

/* Number of times we had to wait for the tail to write */
static ssize_t pool_stats_print_slab_journal_tail_busy_count(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_journal.tail_busy_count);
	#else
	return sprintf(buf, "%lu\n", stats->slab_journal.tail_busy_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_journal_tail_busy_count = {
	.attr = { .name = "slab_journal_tail_busy_count", .mode = 0444, },
	.print = pool_stats_print_slab_journal_tail_busy_count,
};

/* Number of blocks written */
static ssize_t pool_stats_print_slab_summary_blocks_written(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->slab_summary.blocks_written);
	#else
	return sprintf(buf, "%lu\n", stats->slab_summary.blocks_written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_slab_summary_blocks_written = {
	.attr = { .name = "slab_summary_blocks_written", .mode = 0444, },
	.print = pool_stats_print_slab_summary_blocks_written,
};

/* Number of reference blocks written */
static ssize_t pool_stats_print_ref_counts_blocks_written(struct vdo_statistics *stats,
							  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->ref_counts.blocks_written);
	#else
	return sprintf(buf, "%lu\n", stats->ref_counts.blocks_written);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_ref_counts_blocks_written = {
	.attr = { .name = "ref_counts_blocks_written", .mode = 0444, },
	.print = pool_stats_print_ref_counts_blocks_written,
};

/* number of dirty (resident) pages */
static ssize_t pool_stats_print_block_map_dirty_pages(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.dirty_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.dirty_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_dirty_pages = {
	.attr = { .name = "block_map_dirty_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_dirty_pages,
};

/* number of clean (resident) pages */
static ssize_t pool_stats_print_block_map_clean_pages(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.clean_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.clean_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_clean_pages = {
	.attr = { .name = "block_map_clean_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_clean_pages,
};

/* number of free pages */
static ssize_t pool_stats_print_block_map_free_pages(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.free_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.free_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_free_pages = {
	.attr = { .name = "block_map_free_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_free_pages,
};

/* number of pages in failed state */
static ssize_t pool_stats_print_block_map_failed_pages(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.failed_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.failed_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_failed_pages = {
	.attr = { .name = "block_map_failed_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_failed_pages,
};

/* number of pages incoming */
static ssize_t pool_stats_print_block_map_incoming_pages(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.incoming_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.incoming_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_incoming_pages = {
	.attr = { .name = "block_map_incoming_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_incoming_pages,
};

/* number of pages outgoing */
static ssize_t pool_stats_print_block_map_outgoing_pages(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.outgoing_pages);
	#else
	return sprintf(buf, "%u\n", stats->block_map.outgoing_pages);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_outgoing_pages = {
	.attr = { .name = "block_map_outgoing_pages", .mode = 0444, },
	.print = pool_stats_print_block_map_outgoing_pages,
};

/* how many times free page not avail */
static ssize_t pool_stats_print_block_map_cache_pressure(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->block_map.cache_pressure);
	#else
	return sprintf(buf, "%u\n", stats->block_map.cache_pressure);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_cache_pressure = {
	.attr = { .name = "block_map_cache_pressure", .mode = 0444, },
	.print = pool_stats_print_block_map_cache_pressure,
};

/* number of get_vdo_page() calls for read */
static ssize_t pool_stats_print_block_map_read_count(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.read_count);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.read_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_read_count = {
	.attr = { .name = "block_map_read_count", .mode = 0444, },
	.print = pool_stats_print_block_map_read_count,
};

/* number of get_vdo_page() calls for write */
static ssize_t pool_stats_print_block_map_write_count(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.write_count);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.write_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_write_count = {
	.attr = { .name = "block_map_write_count", .mode = 0444, },
	.print = pool_stats_print_block_map_write_count,
};

/* number of times pages failed to read */
static ssize_t pool_stats_print_block_map_failed_reads(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.failed_reads);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.failed_reads);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_failed_reads = {
	.attr = { .name = "block_map_failed_reads", .mode = 0444, },
	.print = pool_stats_print_block_map_failed_reads,
};

/* number of times pages failed to write */
static ssize_t pool_stats_print_block_map_failed_writes(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.failed_writes);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.failed_writes);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_failed_writes = {
	.attr = { .name = "block_map_failed_writes", .mode = 0444, },
	.print = pool_stats_print_block_map_failed_writes,
};

/* number of gets that are reclaimed */
static ssize_t pool_stats_print_block_map_reclaimed(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.reclaimed);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.reclaimed);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_reclaimed = {
	.attr = { .name = "block_map_reclaimed", .mode = 0444, },
	.print = pool_stats_print_block_map_reclaimed,
};

/* number of gets for outgoing pages */
static ssize_t pool_stats_print_block_map_read_outgoing(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.read_outgoing);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.read_outgoing);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_read_outgoing = {
	.attr = { .name = "block_map_read_outgoing", .mode = 0444, },
	.print = pool_stats_print_block_map_read_outgoing,
};

/* number of gets that were already there */
static ssize_t pool_stats_print_block_map_found_in_cache(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.found_in_cache);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.found_in_cache);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_found_in_cache = {
	.attr = { .name = "block_map_found_in_cache", .mode = 0444, },
	.print = pool_stats_print_block_map_found_in_cache,
};

/* number of gets requiring discard */
static ssize_t pool_stats_print_block_map_discard_required(struct vdo_statistics *stats,
							   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.discard_required);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.discard_required);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_discard_required = {
	.attr = { .name = "block_map_discard_required", .mode = 0444, },
	.print = pool_stats_print_block_map_discard_required,
};

/* number of gets enqueued for their page */
static ssize_t pool_stats_print_block_map_wait_for_page(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.wait_for_page);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.wait_for_page);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_wait_for_page = {
	.attr = { .name = "block_map_wait_for_page", .mode = 0444, },
	.print = pool_stats_print_block_map_wait_for_page,
};

/* number of gets that have to fetch */
static ssize_t pool_stats_print_block_map_fetch_required(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.fetch_required);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.fetch_required);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_fetch_required = {
	.attr = { .name = "block_map_fetch_required", .mode = 0444, },
	.print = pool_stats_print_block_map_fetch_required,
};

/* number of page fetches */
static ssize_t pool_stats_print_block_map_pages_loaded(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.pages_loaded);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.pages_loaded);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_pages_loaded = {
	.attr = { .name = "block_map_pages_loaded", .mode = 0444, },
	.print = pool_stats_print_block_map_pages_loaded,
};

/* number of page saves */
static ssize_t pool_stats_print_block_map_pages_saved(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.pages_saved);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.pages_saved);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_pages_saved = {
	.attr = { .name = "block_map_pages_saved", .mode = 0444, },
	.print = pool_stats_print_block_map_pages_saved,
};

/* the number of flushes issued */
static ssize_t pool_stats_print_block_map_flush_count(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->block_map.flush_count);
	#else
	return sprintf(buf, "%lu\n", stats->block_map.flush_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_block_map_flush_count = {
	.attr = { .name = "block_map_flush_count", .mode = 0444, },
	.print = pool_stats_print_block_map_flush_count,
};

/* Number of times the UDS advice proved correct */
static ssize_t pool_stats_print_hash_lock_dedupe_advice_valid(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->hash_lock.dedupe_advice_valid);
	#else
	return sprintf(buf, "%lu\n", stats->hash_lock.dedupe_advice_valid);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_hash_lock_dedupe_advice_valid = {
	.attr = { .name = "hash_lock_dedupe_advice_valid", .mode = 0444, },
	.print = pool_stats_print_hash_lock_dedupe_advice_valid,
};

/* Number of times the UDS advice proved incorrect */
static ssize_t pool_stats_print_hash_lock_dedupe_advice_stale(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->hash_lock.dedupe_advice_stale);
	#else
	return sprintf(buf, "%lu\n", stats->hash_lock.dedupe_advice_stale);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_hash_lock_dedupe_advice_stale = {
	.attr = { .name = "hash_lock_dedupe_advice_stale", .mode = 0444, },
	.print = pool_stats_print_hash_lock_dedupe_advice_stale,
};

/* Number of writes with the same data as another in-flight write */
static ssize_t pool_stats_print_hash_lock_concurrent_data_matches(struct vdo_statistics *stats,
								  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->hash_lock.concurrent_data_matches);
	#else
	return sprintf(buf, "%lu\n", stats->hash_lock.concurrent_data_matches);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_hash_lock_concurrent_data_matches = {
	.attr = { .name = "hash_lock_concurrent_data_matches", .mode = 0444, },
	.print = pool_stats_print_hash_lock_concurrent_data_matches,
};

/* Number of writes whose hash collided with an in-flight write */
static ssize_t pool_stats_print_hash_lock_concurrent_hash_collisions(struct vdo_statistics *stats,
								     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->hash_lock.concurrent_hash_collisions);
	#else
	return sprintf(buf, "%lu\n", stats->hash_lock.concurrent_hash_collisions);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_hash_lock_concurrent_hash_collisions = {
	.attr = { .name = "hash_lock_concurrent_hash_collisions", .mode = 0444, },
	.print = pool_stats_print_hash_lock_concurrent_hash_collisions,
};

/* Current number of dedupe queries that are in flight */
static ssize_t pool_stats_print_hash_lock_curr_dedupe_queries(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->hash_lock.curr_dedupe_queries);
	#else
	return sprintf(buf, "%u\n", stats->hash_lock.curr_dedupe_queries);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_hash_lock_curr_dedupe_queries = {
	.attr = { .name = "hash_lock_curr_dedupe_queries", .mode = 0444, },
	.print = pool_stats_print_hash_lock_curr_dedupe_queries,
};

/* number of times VDO got an invalid dedupe advice PBN from UDS */
static ssize_t pool_stats_print_errors_invalid_advice_pbn_count(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->errors.invalid_advice_pbn_count);
	#else
	return sprintf(buf, "%lu\n", stats->errors.invalid_advice_pbn_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_errors_invalid_advice_pbn_count = {
	.attr = { .name = "errors_invalid_advice_pbn_count", .mode = 0444, },
	.print = pool_stats_print_errors_invalid_advice_pbn_count,
};

/* number of times a VIO completed with a VDO_NO_SPACE error */
static ssize_t pool_stats_print_errors_no_space_error_count(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->errors.no_space_error_count);
	#else
	return sprintf(buf, "%lu\n", stats->errors.no_space_error_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_errors_no_space_error_count = {
	.attr = { .name = "errors_no_space_error_count", .mode = 0444, },
	.print = pool_stats_print_errors_no_space_error_count,
};

/* number of times a VIO completed with a VDO_READ_ONLY error */
static ssize_t pool_stats_print_errors_read_only_error_count(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->errors.read_only_error_count);
	#else
	return sprintf(buf, "%lu\n", stats->errors.read_only_error_count);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_errors_read_only_error_count = {
	.attr = { .name = "errors_read_only_error_count", .mode = 0444, },
	.print = pool_stats_print_errors_read_only_error_count,
};

/* The VDO instance */
static ssize_t pool_stats_print_instance(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->instance);
	#else
	return sprintf(buf, "%u\n", stats->instance);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_instance = {
	.attr = { .name = "instance", .mode = 0444, },
	.print = pool_stats_print_instance,
};

/* Current number of active VIOs */
static ssize_t pool_stats_print_current_vios_in_progress(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->current_vios_in_progress);
	#else
	return sprintf(buf, "%u\n", stats->current_vios_in_progress);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_current_vios_in_progress = {
	.attr = { .name = "current_vios_in_progress", .mode = 0444, },
	.print = pool_stats_print_current_vios_in_progress,
};

/* Maximum number of active VIOs */
static ssize_t pool_stats_print_max_vios(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%u\n", stats->max_vios);
	#else
	return sprintf(buf, "%u\n", stats->max_vios);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_max_vios = {
	.attr = { .name = "max_vios", .mode = 0444, },
	.print = pool_stats_print_max_vios,
};

/* Number of times the UDS index was too slow in responding */
static ssize_t pool_stats_print_dedupe_advice_timeouts(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->dedupe_advice_timeouts);
	#else
	return sprintf(buf, "%lu\n", stats->dedupe_advice_timeouts);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_dedupe_advice_timeouts = {
	.attr = { .name = "dedupe_advice_timeouts", .mode = 0444, },
	.print = pool_stats_print_dedupe_advice_timeouts,
};

/* Number of flush requests submitted to the storage device */
static ssize_t pool_stats_print_flush_out(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->flush_out);
	#else
	return sprintf(buf, "%lu\n", stats->flush_out);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_flush_out = {
	.attr = { .name = "flush_out", .mode = 0444, },
	.print = pool_stats_print_flush_out,
};

/* Logical block size */
static ssize_t pool_stats_print_logical_block_size(struct vdo_statistics *stats,
						   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->logical_block_size);
	#else
	return sprintf(buf, "%lu\n", stats->logical_block_size);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_logical_block_size = {
	.attr = { .name = "logical_block_size", .mode = 0444, },
	.print = pool_stats_print_logical_block_size,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_in_read(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_read = {
	.attr = { .name = "bios_in_read", .mode = 0444, },
	.print = pool_stats_print_bios_in_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_in_write(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_write = {
	.attr = { .name = "bios_in_write", .mode = 0444, },
	.print = pool_stats_print_bios_in_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_in_empty_flush(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_empty_flush = {
	.attr = { .name = "bios_in_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_in_discard(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_discard = {
	.attr = { .name = "bios_in_discard", .mode = 0444, },
	.print = pool_stats_print_bios_in_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_in_flush(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_flush = {
	.attr = { .name = "bios_in_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_in_fua(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_fua = {
	.attr = { .name = "bios_in_fua", .mode = 0444, },
	.print = pool_stats_print_bios_in_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_in_partial_read(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_read = {
	.attr = { .name = "bios_in_partial_read", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_in_partial_write(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_write = {
	.attr = { .name = "bios_in_partial_write", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_in_partial_empty_flush(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_empty_flush = {
	.attr = { .name = "bios_in_partial_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_in_partial_discard(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_discard = {
	.attr = { .name = "bios_in_partial_discard", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_in_partial_flush(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_flush = {
	.attr = { .name = "bios_in_partial_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_in_partial_fua(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_partial.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_partial.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_partial_fua = {
	.attr = { .name = "bios_in_partial_fua", .mode = 0444, },
	.print = pool_stats_print_bios_in_partial_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_out_read(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_read = {
	.attr = { .name = "bios_out_read", .mode = 0444, },
	.print = pool_stats_print_bios_out_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_out_write(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_write = {
	.attr = { .name = "bios_out_write", .mode = 0444, },
	.print = pool_stats_print_bios_out_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_out_empty_flush(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_empty_flush = {
	.attr = { .name = "bios_out_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_out_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_out_discard(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_discard = {
	.attr = { .name = "bios_out_discard", .mode = 0444, },
	.print = pool_stats_print_bios_out_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_out_flush(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_flush = {
	.attr = { .name = "bios_out_flush", .mode = 0444, },
	.print = pool_stats_print_bios_out_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_out_fua(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_fua = {
	.attr = { .name = "bios_out_fua", .mode = 0444, },
	.print = pool_stats_print_bios_out_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_meta_read(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_read = {
	.attr = { .name = "bios_meta_read", .mode = 0444, },
	.print = pool_stats_print_bios_meta_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_meta_write(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_write = {
	.attr = { .name = "bios_meta_write", .mode = 0444, },
	.print = pool_stats_print_bios_meta_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_meta_empty_flush(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_empty_flush = {
	.attr = { .name = "bios_meta_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_meta_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_meta_discard(struct vdo_statistics *stats,
						  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_discard = {
	.attr = { .name = "bios_meta_discard", .mode = 0444, },
	.print = pool_stats_print_bios_meta_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_meta_flush(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_flush = {
	.attr = { .name = "bios_meta_flush", .mode = 0444, },
	.print = pool_stats_print_bios_meta_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_meta_fua(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_fua = {
	.attr = { .name = "bios_meta_fua", .mode = 0444, },
	.print = pool_stats_print_bios_meta_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_journal_read(struct vdo_statistics *stats,
						  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_read = {
	.attr = { .name = "bios_journal_read", .mode = 0444, },
	.print = pool_stats_print_bios_journal_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_journal_write(struct vdo_statistics *stats,
						   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_write = {
	.attr = { .name = "bios_journal_write", .mode = 0444, },
	.print = pool_stats_print_bios_journal_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_journal_empty_flush(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_empty_flush = {
	.attr = { .name = "bios_journal_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_journal_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_journal_discard(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_discard = {
	.attr = { .name = "bios_journal_discard", .mode = 0444, },
	.print = pool_stats_print_bios_journal_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_journal_flush(struct vdo_statistics *stats,
						   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_flush = {
	.attr = { .name = "bios_journal_flush", .mode = 0444, },
	.print = pool_stats_print_bios_journal_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_journal_fua(struct vdo_statistics *stats, char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_fua = {
	.attr = { .name = "bios_journal_fua", .mode = 0444, },
	.print = pool_stats_print_bios_journal_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_page_cache_read(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_read = {
	.attr = { .name = "bios_page_cache_read", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_page_cache_write(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_write = {
	.attr = { .name = "bios_page_cache_write", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_page_cache_empty_flush(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_empty_flush = {
	.attr = { .name = "bios_page_cache_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_page_cache_discard(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_discard = {
	.attr = { .name = "bios_page_cache_discard", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_page_cache_flush(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_flush = {
	.attr = { .name = "bios_page_cache_flush", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_page_cache_fua(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_fua = {
	.attr = { .name = "bios_page_cache_fua", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_out_completed_read(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_read = {
	.attr = { .name = "bios_out_completed_read", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_out_completed_write(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_write = {
	.attr = { .name = "bios_out_completed_write", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_out_completed_empty_flush(struct vdo_statistics *stats,
							       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_empty_flush = {
	.attr = { .name = "bios_out_completed_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_out_completed_discard(struct vdo_statistics *stats,
							   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_discard = {
	.attr = { .name = "bios_out_completed_discard", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_out_completed_flush(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_flush = {
	.attr = { .name = "bios_out_completed_flush", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_out_completed_fua(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_out_completed.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_out_completed.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_out_completed_fua = {
	.attr = { .name = "bios_out_completed_fua", .mode = 0444, },
	.print = pool_stats_print_bios_out_completed_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_meta_completed_read(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_read = {
	.attr = { .name = "bios_meta_completed_read", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_meta_completed_write(struct vdo_statistics *stats,
							  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_write = {
	.attr = { .name = "bios_meta_completed_write", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_meta_completed_empty_flush(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_empty_flush = {
	.attr = { .name = "bios_meta_completed_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_meta_completed_discard(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_discard = {
	.attr = { .name = "bios_meta_completed_discard", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_meta_completed_flush(struct vdo_statistics *stats,
							  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_flush = {
	.attr = { .name = "bios_meta_completed_flush", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_meta_completed_fua(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_meta_completed.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_meta_completed.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_meta_completed_fua = {
	.attr = { .name = "bios_meta_completed_fua", .mode = 0444, },
	.print = pool_stats_print_bios_meta_completed_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_journal_completed_read(struct vdo_statistics *stats,
							    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_read = {
	.attr = { .name = "bios_journal_completed_read", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_journal_completed_write(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_write = {
	.attr = { .name = "bios_journal_completed_write", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_journal_completed_empty_flush(struct vdo_statistics *stats,
								   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_empty_flush = {
	.attr = { .name = "bios_journal_completed_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_journal_completed_discard(struct vdo_statistics *stats,
							       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_discard = {
	.attr = { .name = "bios_journal_completed_discard", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_journal_completed_flush(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_flush = {
	.attr = { .name = "bios_journal_completed_flush", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_journal_completed_fua(struct vdo_statistics *stats,
							   char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_journal_completed.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_journal_completed.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_journal_completed_fua = {
	.attr = { .name = "bios_journal_completed_fua", .mode = 0444, },
	.print = pool_stats_print_bios_journal_completed_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_page_cache_completed_read(struct vdo_statistics *stats,
							       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_read = {
	.attr = { .name = "bios_page_cache_completed_read", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_page_cache_completed_write(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_write = {
	.attr = { .name = "bios_page_cache_completed_write", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_page_cache_completed_empty_flush(struct vdo_statistics *stats,
								      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_empty_flush = {
	.attr = { .name = "bios_page_cache_completed_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_page_cache_completed_discard(struct vdo_statistics *stats,
								  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_discard = {
	.attr = { .name = "bios_page_cache_completed_discard", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_page_cache_completed_flush(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_flush = {
	.attr = { .name = "bios_page_cache_completed_flush", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_page_cache_completed_fua(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_page_cache_completed.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_page_cache_completed.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_page_cache_completed_fua = {
	.attr = { .name = "bios_page_cache_completed_fua", .mode = 0444, },
	.print = pool_stats_print_bios_page_cache_completed_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_acknowledged_read(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_read = {
	.attr = { .name = "bios_acknowledged_read", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_acknowledged_write(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_write = {
	.attr = { .name = "bios_acknowledged_write", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_acknowledged_empty_flush(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_empty_flush = {
	.attr = { .name = "bios_acknowledged_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_acknowledged_discard(struct vdo_statistics *stats,
							  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_discard = {
	.attr = { .name = "bios_acknowledged_discard", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_acknowledged_flush(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_flush = {
	.attr = { .name = "bios_acknowledged_flush", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_acknowledged_fua(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_fua = {
	.attr = { .name = "bios_acknowledged_fua", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_acknowledged_partial_read(struct vdo_statistics *stats,
							       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_read = {
	.attr = { .name = "bios_acknowledged_partial_read", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_acknowledged_partial_write(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_write = {
	.attr = { .name = "bios_acknowledged_partial_write", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_acknowledged_partial_empty_flush(struct vdo_statistics *stats,
								      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_empty_flush = {
	.attr = { .name = "bios_acknowledged_partial_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_acknowledged_partial_discard(struct vdo_statistics *stats,
								  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_discard = {
	.attr = { .name = "bios_acknowledged_partial_discard", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_acknowledged_partial_flush(struct vdo_statistics *stats,
								char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_flush = {
	.attr = { .name = "bios_acknowledged_partial_flush", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_acknowledged_partial_fua(struct vdo_statistics *stats,
							      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_acknowledged_partial.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_acknowledged_partial.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_acknowledged_partial_fua = {
	.attr = { .name = "bios_acknowledged_partial_fua", .mode = 0444, },
	.print = pool_stats_print_bios_acknowledged_partial_fua,
};

/* Number of REQ_OP_READ bios */
static ssize_t pool_stats_print_bios_in_progress_read(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.read);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.read);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_read = {
	.attr = { .name = "bios_in_progress_read", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_read,
};

/* Number of REQ_OP_WRITE bios with data */
static ssize_t pool_stats_print_bios_in_progress_write(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.write);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.write);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_write = {
	.attr = { .name = "bios_in_progress_write", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_write,
};

/* Number of bios tagged with REQ_PREFLUSH and containing no data */
static ssize_t pool_stats_print_bios_in_progress_empty_flush(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.empty_flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.empty_flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_empty_flush = {
	.attr = { .name = "bios_in_progress_empty_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_empty_flush,
};

/* Number of REQ_OP_DISCARD bios */
static ssize_t pool_stats_print_bios_in_progress_discard(struct vdo_statistics *stats,
							 char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.discard);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.discard);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_discard = {
	.attr = { .name = "bios_in_progress_discard", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_discard,
};

/* Number of bios tagged with REQ_PREFLUSH */
static ssize_t pool_stats_print_bios_in_progress_flush(struct vdo_statistics *stats,
						       char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.flush);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.flush);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_flush = {
	.attr = { .name = "bios_in_progress_flush", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_flush,
};

/* Number of bios tagged with REQ_FUA */
static ssize_t pool_stats_print_bios_in_progress_fua(struct vdo_statistics *stats,
						     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->bios_in_progress.fua);
	#else
	return sprintf(buf, "%lu\n", stats->bios_in_progress.fua);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_bios_in_progress_fua = {
	.attr = { .name = "bios_in_progress_fua", .mode = 0444, },
	.print = pool_stats_print_bios_in_progress_fua,
};

/* Tracked bytes currently allocated. */
static ssize_t pool_stats_print_memory_usage_bytes_used(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->memory_usage.bytes_used);
	#else
	return sprintf(buf, "%lu\n", stats->memory_usage.bytes_used);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_memory_usage_bytes_used = {
	.attr = { .name = "memory_usage_bytes_used", .mode = 0444, },
	.print = pool_stats_print_memory_usage_bytes_used,
};

/* Maximum tracked bytes allocated. */
static ssize_t pool_stats_print_memory_usage_peak_bytes_used(struct vdo_statistics *stats,
							     char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->memory_usage.peak_bytes_used);
	#else
	return sprintf(buf, "%lu\n", stats->memory_usage.peak_bytes_used);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_memory_usage_peak_bytes_used = {
	.attr = { .name = "memory_usage_peak_bytes_used", .mode = 0444, },
	.print = pool_stats_print_memory_usage_peak_bytes_used,
};

/* Number of records stored in the index */
static ssize_t pool_stats_print_index_entries_indexed(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.entries_indexed);
	#else
	return sprintf(buf, "%lu\n", stats->index.entries_indexed);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_entries_indexed = {
	.attr = { .name = "index_entries_indexed", .mode = 0444, },
	.print = pool_stats_print_index_entries_indexed,
};

/* Number of post calls that found an existing entry */
static ssize_t pool_stats_print_index_posts_found(struct vdo_statistics *stats,
						  char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.posts_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.posts_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_posts_found = {
	.attr = { .name = "index_posts_found", .mode = 0444, },
	.print = pool_stats_print_index_posts_found,
};

/* Number of post calls that added a new entry */
static ssize_t pool_stats_print_index_posts_not_found(struct vdo_statistics *stats,
						      char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.posts_not_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.posts_not_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_posts_not_found = {
	.attr = { .name = "index_posts_not_found", .mode = 0444, },
	.print = pool_stats_print_index_posts_not_found,
};

/* Number of query calls that found an existing entry */
static ssize_t pool_stats_print_index_queries_found(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.queries_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.queries_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_queries_found = {
	.attr = { .name = "index_queries_found", .mode = 0444, },
	.print = pool_stats_print_index_queries_found,
};

/* Number of query calls that added a new entry */
static ssize_t pool_stats_print_index_queries_not_found(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.queries_not_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.queries_not_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_queries_not_found = {
	.attr = { .name = "index_queries_not_found", .mode = 0444, },
	.print = pool_stats_print_index_queries_not_found,
};

/* Number of update calls that found an existing entry */
static ssize_t pool_stats_print_index_updates_found(struct vdo_statistics *stats,
						    char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.updates_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.updates_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_updates_found = {
	.attr = { .name = "index_updates_found", .mode = 0444, },
	.print = pool_stats_print_index_updates_found,
};

/* Number of update calls that added a new entry */
static ssize_t pool_stats_print_index_updates_not_found(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.updates_not_found);
	#else
	return sprintf(buf, "%lu\n", stats->index.updates_not_found);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_updates_not_found = {
	.attr = { .name = "index_updates_not_found", .mode = 0444, },
	.print = pool_stats_print_index_updates_not_found,
};

/* Number of entries discarded */
static ssize_t pool_stats_print_index_entries_discarded(struct vdo_statistics *stats,
							char *buf)
{
	#ifdef __KERNEL__
	return sprintf(buf, "%llu\n", stats->index.entries_discarded);
	#else
	return sprintf(buf, "%lu\n", stats->index.entries_discarded);
	#endif // __KERNEL__
}

static struct pool_stats_attribute pool_stats_attr_index_entries_discarded = {
	.attr = { .name = "index_entries_discarded", .mode = 0444, },
	.print = pool_stats_print_index_entries_discarded,
};

struct attribute *vdo_pool_stats_attrs[] = {
	&pool_stats_attr_data_blocks_used.attr,
	&pool_stats_attr_overhead_blocks_used.attr,
	&pool_stats_attr_logical_blocks_used.attr,
	&pool_stats_attr_physical_blocks.attr,
	&pool_stats_attr_logical_blocks.attr,
	&pool_stats_attr_block_map_cache_size.attr,
	&pool_stats_attr_block_size.attr,
	&pool_stats_attr_complete_recoveries.attr,
	&pool_stats_attr_read_only_recoveries.attr,
	&pool_stats_attr_mode.attr,
	&pool_stats_attr_in_recovery_mode.attr,
	&pool_stats_attr_recovery_percentage.attr,
	&pool_stats_attr_packer_compressed_fragments_written.attr,
	&pool_stats_attr_packer_compressed_blocks_written.attr,
	&pool_stats_attr_packer_compressed_fragments_in_packer.attr,
	&pool_stats_attr_allocator_slab_count.attr,
	&pool_stats_attr_allocator_slabs_opened.attr,
	&pool_stats_attr_allocator_slabs_reopened.attr,
	&pool_stats_attr_journal_disk_full.attr,
	&pool_stats_attr_journal_slab_journal_commits_requested.attr,
	&pool_stats_attr_journal_entries_started.attr,
	&pool_stats_attr_journal_entries_written.attr,
	&pool_stats_attr_journal_entries_committed.attr,
	&pool_stats_attr_journal_blocks_started.attr,
	&pool_stats_attr_journal_blocks_written.attr,
	&pool_stats_attr_journal_blocks_committed.attr,
	&pool_stats_attr_slab_journal_disk_full_count.attr,
	&pool_stats_attr_slab_journal_flush_count.attr,
	&pool_stats_attr_slab_journal_blocked_count.attr,
	&pool_stats_attr_slab_journal_blocks_written.attr,
	&pool_stats_attr_slab_journal_tail_busy_count.attr,
	&pool_stats_attr_slab_summary_blocks_written.attr,
	&pool_stats_attr_ref_counts_blocks_written.attr,
	&pool_stats_attr_block_map_dirty_pages.attr,
	&pool_stats_attr_block_map_clean_pages.attr,
	&pool_stats_attr_block_map_free_pages.attr,
	&pool_stats_attr_block_map_failed_pages.attr,
	&pool_stats_attr_block_map_incoming_pages.attr,
	&pool_stats_attr_block_map_outgoing_pages.attr,
	&pool_stats_attr_block_map_cache_pressure.attr,
	&pool_stats_attr_block_map_read_count.attr,
	&pool_stats_attr_block_map_write_count.attr,
	&pool_stats_attr_block_map_failed_reads.attr,
	&pool_stats_attr_block_map_failed_writes.attr,
	&pool_stats_attr_block_map_reclaimed.attr,
	&pool_stats_attr_block_map_read_outgoing.attr,
	&pool_stats_attr_block_map_found_in_cache.attr,
	&pool_stats_attr_block_map_discard_required.attr,
	&pool_stats_attr_block_map_wait_for_page.attr,
	&pool_stats_attr_block_map_fetch_required.attr,
	&pool_stats_attr_block_map_pages_loaded.attr,
	&pool_stats_attr_block_map_pages_saved.attr,
	&pool_stats_attr_block_map_flush_count.attr,
	&pool_stats_attr_hash_lock_dedupe_advice_valid.attr,
	&pool_stats_attr_hash_lock_dedupe_advice_stale.attr,
	&pool_stats_attr_hash_lock_concurrent_data_matches.attr,
	&pool_stats_attr_hash_lock_concurrent_hash_collisions.attr,
	&pool_stats_attr_hash_lock_curr_dedupe_queries.attr,
	&pool_stats_attr_errors_invalid_advice_pbn_count.attr,
	&pool_stats_attr_errors_no_space_error_count.attr,
	&pool_stats_attr_errors_read_only_error_count.attr,
	&pool_stats_attr_instance.attr,
	&pool_stats_attr_current_vios_in_progress.attr,
	&pool_stats_attr_max_vios.attr,
	&pool_stats_attr_dedupe_advice_timeouts.attr,
	&pool_stats_attr_flush_out.attr,
	&pool_stats_attr_logical_block_size.attr,
	&pool_stats_attr_bios_in_read.attr,
	&pool_stats_attr_bios_in_write.attr,
	&pool_stats_attr_bios_in_empty_flush.attr,
	&pool_stats_attr_bios_in_discard.attr,
	&pool_stats_attr_bios_in_flush.attr,
	&pool_stats_attr_bios_in_fua.attr,
	&pool_stats_attr_bios_in_partial_read.attr,
	&pool_stats_attr_bios_in_partial_write.attr,
	&pool_stats_attr_bios_in_partial_empty_flush.attr,
	&pool_stats_attr_bios_in_partial_discard.attr,
	&pool_stats_attr_bios_in_partial_flush.attr,
	&pool_stats_attr_bios_in_partial_fua.attr,
	&pool_stats_attr_bios_out_read.attr,
	&pool_stats_attr_bios_out_write.attr,
	&pool_stats_attr_bios_out_empty_flush.attr,
	&pool_stats_attr_bios_out_discard.attr,
	&pool_stats_attr_bios_out_flush.attr,
	&pool_stats_attr_bios_out_fua.attr,
	&pool_stats_attr_bios_meta_read.attr,
	&pool_stats_attr_bios_meta_write.attr,
	&pool_stats_attr_bios_meta_empty_flush.attr,
	&pool_stats_attr_bios_meta_discard.attr,
	&pool_stats_attr_bios_meta_flush.attr,
	&pool_stats_attr_bios_meta_fua.attr,
	&pool_stats_attr_bios_journal_read.attr,
	&pool_stats_attr_bios_journal_write.attr,
	&pool_stats_attr_bios_journal_empty_flush.attr,
	&pool_stats_attr_bios_journal_discard.attr,
	&pool_stats_attr_bios_journal_flush.attr,
	&pool_stats_attr_bios_journal_fua.attr,
	&pool_stats_attr_bios_page_cache_read.attr,
	&pool_stats_attr_bios_page_cache_write.attr,
	&pool_stats_attr_bios_page_cache_empty_flush.attr,
	&pool_stats_attr_bios_page_cache_discard.attr,
	&pool_stats_attr_bios_page_cache_flush.attr,
	&pool_stats_attr_bios_page_cache_fua.attr,
	&pool_stats_attr_bios_out_completed_read.attr,
	&pool_stats_attr_bios_out_completed_write.attr,
	&pool_stats_attr_bios_out_completed_empty_flush.attr,
	&pool_stats_attr_bios_out_completed_discard.attr,
	&pool_stats_attr_bios_out_completed_flush.attr,
	&pool_stats_attr_bios_out_completed_fua.attr,
	&pool_stats_attr_bios_meta_completed_read.attr,
	&pool_stats_attr_bios_meta_completed_write.attr,
	&pool_stats_attr_bios_meta_completed_empty_flush.attr,
	&pool_stats_attr_bios_meta_completed_discard.attr,
	&pool_stats_attr_bios_meta_completed_flush.attr,
	&pool_stats_attr_bios_meta_completed_fua.attr,
	&pool_stats_attr_bios_journal_completed_read.attr,
	&pool_stats_attr_bios_journal_completed_write.attr,
	&pool_stats_attr_bios_journal_completed_empty_flush.attr,
	&pool_stats_attr_bios_journal_completed_discard.attr,
	&pool_stats_attr_bios_journal_completed_flush.attr,
	&pool_stats_attr_bios_journal_completed_fua.attr,
	&pool_stats_attr_bios_page_cache_completed_read.attr,
	&pool_stats_attr_bios_page_cache_completed_write.attr,
	&pool_stats_attr_bios_page_cache_completed_empty_flush.attr,
	&pool_stats_attr_bios_page_cache_completed_discard.attr,
	&pool_stats_attr_bios_page_cache_completed_flush.attr,
	&pool_stats_attr_bios_page_cache_completed_fua.attr,
	&pool_stats_attr_bios_acknowledged_read.attr,
	&pool_stats_attr_bios_acknowledged_write.attr,
	&pool_stats_attr_bios_acknowledged_empty_flush.attr,
	&pool_stats_attr_bios_acknowledged_discard.attr,
	&pool_stats_attr_bios_acknowledged_flush.attr,
	&pool_stats_attr_bios_acknowledged_fua.attr,
	&pool_stats_attr_bios_acknowledged_partial_read.attr,
	&pool_stats_attr_bios_acknowledged_partial_write.attr,
	&pool_stats_attr_bios_acknowledged_partial_empty_flush.attr,
	&pool_stats_attr_bios_acknowledged_partial_discard.attr,
	&pool_stats_attr_bios_acknowledged_partial_flush.attr,
	&pool_stats_attr_bios_acknowledged_partial_fua.attr,
	&pool_stats_attr_bios_in_progress_read.attr,
	&pool_stats_attr_bios_in_progress_write.attr,
	&pool_stats_attr_bios_in_progress_empty_flush.attr,
	&pool_stats_attr_bios_in_progress_discard.attr,
	&pool_stats_attr_bios_in_progress_flush.attr,
	&pool_stats_attr_bios_in_progress_fua.attr,
	&pool_stats_attr_memory_usage_bytes_used.attr,
	&pool_stats_attr_memory_usage_peak_bytes_used.attr,
	&pool_stats_attr_index_entries_indexed.attr,
	&pool_stats_attr_index_posts_found.attr,
	&pool_stats_attr_index_posts_not_found.attr,
	&pool_stats_attr_index_queries_found.attr,
	&pool_stats_attr_index_queries_not_found.attr,
	&pool_stats_attr_index_updates_found.attr,
	&pool_stats_attr_index_updates_not_found.attr,
	&pool_stats_attr_index_entries_discarded.attr,
	NULL,
};
