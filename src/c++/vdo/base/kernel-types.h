/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include "types.h"

#ifdef __KERNEL__
#include <linux/version.h>
#else
#define KERNEL_VERSION(x, y, z) ((x << 16) + (y << 8) + z)
#define LINUX_VERSION_CODE KERNEL_VERSION(5,17,0)
#endif /* not __KERNEL__ */

/**
 * typedef vio_count_t - A number of vios.
 */
typedef uint16_t vio_count_t;

/*
 * vio types for statistics and instrumentation.
 */
enum vio_type {
	VIO_TYPE_UNINITIALIZED = 0,
	VIO_TYPE_DATA,
	VIO_TYPE_BLOCK_ALLOCATOR,
	VIO_TYPE_BLOCK_MAP,
	VIO_TYPE_BLOCK_MAP_INTERIOR,
	VIO_TYPE_PARTITION_COPY,
	VIO_TYPE_RECOVERY_JOURNAL,
	VIO_TYPE_SLAB_JOURNAL,
	VIO_TYPE_SLAB_SUMMARY,
	VIO_TYPE_SUPER_BLOCK,
	VIO_TYPE_TEST,
} __packed;

/**
 * vdo_is_data_vio_type() - Check whether a vio_type is for servicing an
 *                          external data request.
 * @type: The vio_type to check.
 */
static inline bool vdo_is_data_vio_type(enum vio_type type)
{
	return (type == VIO_TYPE_DATA);
}

/**
 * vdo_is_metadata_vio_type() - Check whether a vio_type is for metadata.
 * @type: The vio_type to check.
 */
static inline bool vdo_is_metadata_vio_type(enum vio_type type)
{
	return ((type != VIO_TYPE_UNINITIALIZED) &&
		!vdo_is_data_vio_type(type));
}

enum vdo_completion_priority {
	BIO_ACK_Q_ACK_PRIORITY = 0,
	BIO_ACK_Q_MAX_PRIORITY = 0,
	BIO_Q_COMPRESSED_DATA_PRIORITY = 0,
	BIO_Q_DATA_PRIORITY = 0,
	BIO_Q_FLUSH_PRIORITY = 2,
	BIO_Q_HIGH_PRIORITY = 2,
	BIO_Q_METADATA_PRIORITY = 1,
	BIO_Q_VERIFY_PRIORITY = 1,
	BIO_Q_MAX_PRIORITY = 2,
	CPU_Q_COMPLETE_VIO_PRIORITY = 0,
	CPU_Q_COMPLETE_READ_PRIORITY = 0,
	CPU_Q_COMPRESS_BLOCK_PRIORITY = 0,
	CPU_Q_EVENT_REPORTER_PRIORITY = 0,
	CPU_Q_HASH_BLOCK_PRIORITY = 0,
	CPU_Q_MAX_PRIORITY = 0,
	UDS_Q_PRIORITY = 0,
	UDS_Q_MAX_PRIORITY = 0,
	VDO_DEFAULT_Q_COMPLETION_PRIORITY = 1,
	VDO_DEFAULT_Q_FLUSH_PRIORITY = 2,
	VDO_DEFAULT_Q_MAP_BIO_PRIORITY = 0,
	VDO_DEFAULT_Q_SYNC_PRIORITY = 2,
	VDO_DEFAULT_Q_VIO_CALLBACK_PRIORITY = 1,
	VDO_DEFAULT_Q_MAX_PRIORITY = 2,
	/* The maximum allowable priority */
	VDO_WORK_Q_MAX_PRIORITY = 3,
	/* A value which must be out of range for a valid priority */
	VDO_WORK_Q_DEFAULT_PRIORITY = VDO_WORK_Q_MAX_PRIORITY + 1,
};

/*
 * Priority levels for asynchronous I/O operations performed on a vio.
 */
enum vio_priority {
	VIO_PRIORITY_LOW = 0,
	VIO_PRIORITY_DATA = VIO_PRIORITY_LOW,
	VIO_PRIORITY_COMPRESSED_DATA = VIO_PRIORITY_DATA,
	VIO_PRIORITY_METADATA,
	VIO_PRIORITY_HIGH,
} __packed;

enum vdo_zone_type {
	VDO_ZONE_TYPE_ADMIN,
	VDO_ZONE_TYPE_JOURNAL,
	VDO_ZONE_TYPE_LOGICAL,
	VDO_ZONE_TYPE_PHYSICAL,
};

/*
 * Forward declarations of abstract types
 */
struct action_manager;
struct allocation_selector;
struct atomic_bio_stats;
#ifdef INTERNAL
struct bio;
#endif /* INTERNAL */
struct block_allocator;
#ifdef INTERNAL
struct block_device;
#endif /* INTERNAL */
struct block_map;
struct block_map_tree_zone;
struct block_map_zone;
struct data_vio;
struct data_vio_pool;
struct dedupe_context;
struct device_config;
struct flusher;
struct forest;
struct index_config;
struct input_bin;
struct io_submitter;
struct lbn_lock;
struct lock_counter;
struct logical_zone;
struct logical_zones;
struct pbn_lock;
struct physical_zone;
struct physical_zones;
struct read_only_notifier;
struct recovery_journal;
struct ref_counts;
struct slab_depot;
struct slab_journal;
struct slab_journal_entry;
struct slab_scrubber;
struct slab_summary;
struct slab_summary_zone;
struct thread_config;
struct vdo;
struct vdo_completion;
struct vdo_flush;
struct vdo_layout;
struct vdo_slab;
struct vdo_statistics;
struct vdo_thread;
struct vdo_work_queue;
struct vio;
struct vio_pool;

struct zoned_pbn {
	physical_block_number_t pbn;
	enum block_mapping_state state;
	struct physical_zone *zone;
};

#endif /* KERNEL_TYPES_H */
