/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_H
#define VDO_H

#include <linux/atomic.h>
#ifdef __KERNEL__
#include <linux/blk_types.h>
#include <linux/crc32.h>
#endif // __KERNEL__
#include <linux/kobject.h>
#include <linux/list.h>

#ifdef __KERNEL__
#include "thread-registry.h"
#endif /* __KERNEL__ */

#include "admin-completion.h"
#include "admin-state.h"
#include "device-config.h"
#include "header.h"
#include "packer.h"
#include "physical-zone.h"
#include "statistics.h"
#include "super-block.h"
#include "read-only-notifier.h"
#include "thread-config.h"
#include "types.h"
#include "uds.h"
#include "vdo-component.h"
#include "vdo-component-states.h"
#include "vdo-layout.h"
#include "volume-geometry.h"

#ifdef VDO_INTERNAL
#include "vdoHistograms.h"
#endif /* VDO_INTERNAL */

struct vdo_thread {
	struct vdo *vdo;
	thread_id_t thread_id;
	struct vdo_work_queue *queue;
#ifdef __KERNEL__
	struct registered_thread allocating_thread;
#endif /* __KERNEL__ */
};

/* Keep struct bio statistics atomically */
struct atomic_bio_stats {
	atomic64_t read; /* Number of not REQ_WRITE bios */
	atomic64_t write; /* Number of REQ_WRITE bios */
	atomic64_t discard; /* Number of REQ_DISCARD bios */
	atomic64_t flush; /* Number of REQ_FLUSH bios */
	atomic64_t empty_flush; /* Number of REQ_PREFLUSH bios without data */
	atomic64_t fua; /* Number of REQ_FUA bios */
};

/*
 * Counters are atomic since updates can arrive concurrently from arbitrary
 * threads.
 */
struct atomic_statistics {
	atomic64_t bios_submitted;
	atomic64_t bios_completed;
	atomic64_t flush_out;
	atomic64_t invalid_advice_pbn_count;
	atomic64_t no_space_error_count;
	atomic64_t read_only_error_count;
	struct atomic_bio_stats bios_in;
	struct atomic_bio_stats bios_in_partial;
	struct atomic_bio_stats bios_out;
	struct atomic_bio_stats bios_out_completed;
	struct atomic_bio_stats bios_acknowledged;
	struct atomic_bio_stats bios_acknowledged_partial;
	struct atomic_bio_stats bios_meta;
	struct atomic_bio_stats bios_meta_completed;
	struct atomic_bio_stats bios_journal;
	struct atomic_bio_stats bios_journal_completed;
	struct atomic_bio_stats bios_page_cache;
	struct atomic_bio_stats bios_page_cache_completed;
};

struct data_vio_pool;

struct vdo {
	char thread_name_prefix[MAX_VDO_WORK_QUEUE_NAME_LEN];
	struct vdo_thread *threads;
	vdo_action *action;
	struct vdo_completion *completion;
	struct vio_tracer *vio_tracer;

	/* The atomic version of the state of this vdo */
	atomic_t state;
	/* The full state of all components */
	struct vdo_component_states states;
	/*
	 * A counter value to attach to thread names and log messages to
	 * identify the individual device.
	 */
	unsigned int instance;
	/* The read-only notifier */
	struct read_only_notifier *read_only_notifier;
	/* The load-time configuration of this vdo */
	struct device_config *device_config;
	/* The thread mapping */
	struct thread_config *thread_config;

	/* The super block */
	struct vdo_super_block *super_block;

	/* Our partitioning of the physical layer's storage */
	struct vdo_layout *layout;

	/* The block map */
	struct block_map *block_map;

	/* The journal for block map recovery */
	struct recovery_journal *recovery_journal;

	/* The slab depot */
	struct slab_depot *depot;

	/* The compressed-block packer */
	struct packer *packer;
	/* Whether incoming data should be compressed */
	bool compressing;

	/* The handler for flush requests */
	struct flusher *flusher;

	/* The state the vdo was in when loaded (primarily for unit tests) */
	enum vdo_state load_state;

	/* The logical zones of this vdo */
	struct logical_zones *logical_zones;

	/* The physical zones of this vdo */
	struct physical_zones *physical_zones;

	/* The hash lock zones of this vdo */
	struct hash_zones *hash_zones;

	/*
	 * Bio submission manager used for sending bios to the storage
	 * device.
	 */
	struct io_submitter *io_submitter;

	/* The pool of data_vios for servicing incoming bios */
	struct data_vio_pool *data_vio_pool;

	/* The completion for administrative operations */
	struct admin_completion admin_completion;

	/* The administrative state of the vdo */
	struct admin_state admin_state;

	/* Flags controlling administrative operations */
	const struct admin_state_code *suspend_type;
	bool allocations_allowed;
	bool dump_on_shutdown;
	atomic_t processing_message;

	/*
	 * Statistics
	 * Atomic stats counters
	 */
	struct atomic_statistics stats;
	/* Used to gather statistics without allocating memory */
	struct vdo_statistics stats_buffer;
	/* Protects the stats_buffer */
	struct mutex stats_mutex;
	/* true if sysfs directory is set up */
	bool sysfs_added;
	/* Used when shutting down the sysfs statistics */
	struct completion stats_shutdown;

#ifdef VDO_INTERNAL
	struct vdo_histograms histograms;
#endif /* VDO_INTERNAL */

	/* A list of all device_configs referencing this vdo */
	struct list_head device_config_list;

	/* This VDO's list entry for the device registry */
	struct list_head registration;

	/* Underlying block device info. */
	u64 starting_sector_offset;
	struct volume_geometry geometry;

	/* For sysfs */
	struct kobject vdo_directory;
	struct kobject stats_directory;

	/* N blobs of context data for LZ4 code, one per CPU thread. */
	char **compression_context;
};

#if defined(VDO_INTERNAL) || defined(INTERNAL)
extern int data_vio_count;
#endif /* VDO_INTERNAL or INTERNAL */

/**
 * vdo_uses_bio_ack_queue() - Indicate whether the vdo is configured to use a
 *                            separate work queue for acknowledging received
 *                            and processed bios.
 * @vdo: The vdo.
 *
 * Note that this directly controls the handling of write operations, but the
 * compile-time flag VDO_USE_BIO_ACK_QUEUE_FOR_READ is also checked for read
 * operations.
 *
 * Return: Whether a bio-acknowledgement work queue is in use.
 */
static inline bool vdo_uses_bio_ack_queue(struct vdo *vdo)
{
	return vdo->device_config->thread_counts.bio_ack_threads > 0;
}

int __must_check
vdo_make_thread(struct vdo *vdo,
		thread_id_t thread_id,
		const struct vdo_work_queue_type *type,
		unsigned int queue_count,
		void *contexts[]);

static inline int __must_check
vdo_make_default_thread(struct vdo *vdo, thread_id_t thread_id)
{
	return vdo_make_thread(vdo, thread_id, NULL, 1, NULL);
}

int __must_check
vdo_make(unsigned int instance,
	 struct device_config *config,
	 char **reason,
	 struct vdo **vdo_ptr);

void vdo_destroy(struct vdo *vdo);

int __must_check vdo_add_sysfs_stats_dir(struct vdo *vdo);

struct block_device * __must_check
vdo_get_backing_device(const struct vdo *vdo);

const char * __must_check
vdo_get_device_name(const struct dm_target *target);

#ifdef __KERNEL__
int __must_check vdo_synchronous_flush(struct vdo *vdo);
#endif /* __KERNEL__ */

const struct admin_state_code * __must_check
vdo_get_admin_state(const struct vdo *vdo);

bool vdo_set_compressing(struct vdo *vdo, bool enable);

bool vdo_get_compressing(struct vdo *vdo);

void vdo_fetch_statistics(struct vdo *vdo, struct vdo_statistics *stats);

thread_id_t vdo_get_callback_thread_id(void);

enum vdo_state __must_check vdo_get_state(const struct vdo *vdo);

void vdo_set_state(struct vdo *vdo, enum vdo_state state);

void vdo_save_components(struct vdo *vdo, struct vdo_completion *parent);

int vdo_enable_read_only_entry(struct vdo *vdo);

bool __must_check vdo_in_read_only_mode(const struct vdo *vdo);

bool __must_check vdo_in_recovery_mode(const struct vdo *vdo);

void vdo_enter_recovery_mode(struct vdo *vdo);

void vdo_assert_on_admin_thread(const struct vdo *vdo, const char *name);

void vdo_assert_on_logical_zone_thread(const struct vdo *vdo,
				       zone_count_t logical_zone,
				       const char *name);

void vdo_assert_on_physical_zone_thread(const struct vdo *vdo,
					zone_count_t physical_zone,
					const char *name);

static inline void vdo_assert_on_dedupe_thread(const struct vdo *vdo,
					       const char *name) {
	ASSERT_LOG_ONLY((vdo_get_callback_thread_id() ==
			 vdo->thread_config->dedupe_thread),
			"%s called on dedupe index thread",
			name);
}

void assert_on_vdo_cpu_thread(const struct vdo *vdo, const char *name);

int __must_check vdo_get_physical_zone(const struct vdo *vdo,
				       physical_block_number_t pbn,
				       struct physical_zone **zone_ptr);

void vdo_dump_status(const struct vdo *vdo);

#ifdef INTERNAL
block_count_t __must_check
vdo_get_physical_blocks_allocated(const struct vdo *vdo);

block_count_t __must_check
vdo_get_physical_blocks_overhead(const struct vdo *vdo);
#endif /* INTERNAL */

#ifdef __KERNEL__
/*
 * We start with 0L and postcondition with ~0L to match our historical usage
 * in userspace.
 */
static inline u32 vdo_crc32(const void *buf, unsigned long len)
{
	return (crc32(0L, buf, len) ^ ~0L);
}
#endif // __KERNEL__

#endif /* VDO_H */
