// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/module.h>

#ifdef __KERNEL__
#include "dm-vdo/admin-state.h"
#include "dm-vdo/block-map.h"
#include "dm-vdo/completion.h"
#include "dm-vdo/constants.h"
#include "dm-vdo/data-vio.h"
#include "dm-vdo/dedupe.h"
#include "dm-vdo/device-registry.h"
#include "dm-vdo/dump.h"
#include "dm-vdo/flush.h"
#include "dm-vdo/instance-number.h"
#include "dm-vdo/io-submitter.h"
#include "dm-vdo/logger.h"
#include "dm-vdo/memory-alloc.h"
#include "dm-vdo/message-stats.h"
#include "dm-vdo/pool-sysfs.h"
#include "dm-vdo/recovery-journal.h"
#include "dm-vdo/slab-depot.h"
#include "dm-vdo/slab-summary.h"
#include "dm-vdo/string-utils.h"
#include "dm-vdo/super-block-codec.h"
#include "dm-vdo/thread-config.h"
#include "dm-vdo/thread-device.h"
#include "dm-vdo/thread-registry.h"
#include "dm-vdo/uds-sysfs.h"
#include "dm-vdo/vdo.h"
#include "dm-vdo/vdo-recovery.h"
#include "dm-vdo/vio.h"
#else /* not __KERNEL__ */
#include "admin-state.h"
#include "block-map.h"
#include "completion.h"
#include "constants.h"
#include "data-vio.h"
#include "dedupe.h"
#include "device-registry.h"
#include "dump.h"
#include "flush.h"
#include "instance-number.h"
#include "io-submitter.h"
#include "logger.h"
#include "memory-alloc.h"
#include "message-stats.h"
#include "recovery-journal.h"
#include "slab-depot.h"
#include "slab-summary.h"
#include "string-utils.h"
#include "super-block-codec.h"
#include "thread-config.h"
#include "vdo.h"
#include "vdo-recovery.h"
#include "vio.h"
#endif /* __KERNEL__ */

#define	CURRENT_VERSION	VDO_VERSION

enum {
	GROW_LOGICAL_PHASE_START,
	GROW_LOGICAL_PHASE_GROW_BLOCK_MAP,
	GROW_LOGICAL_PHASE_END,
	GROW_LOGICAL_PHASE_ERROR,
	GROW_PHYSICAL_PHASE_START,
	GROW_PHYSICAL_PHASE_COPY_SUMMARY,
	GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS,
	GROW_PHYSICAL_PHASE_USE_NEW_SLABS,
	GROW_PHYSICAL_PHASE_END,
	GROW_PHYSICAL_PHASE_ERROR,
	LOAD_PHASE_START,
	LOAD_PHASE_STATS,
	LOAD_PHASE_LOAD_DEPOT,
	LOAD_PHASE_MAKE_DIRTY,
	LOAD_PHASE_PREPARE_TO_ALLOCATE,
	LOAD_PHASE_SCRUB_SLABS,
	LOAD_PHASE_DATA_REDUCTION,
	LOAD_PHASE_FINISHED,
	LOAD_PHASE_DRAIN_JOURNAL,
	LOAD_PHASE_WAIT_FOR_READ_ONLY,
	PRE_LOAD_PHASE_START,
	PRE_LOAD_PHASE_LOAD_COMPONENTS,
	PRE_LOAD_PHASE_END,
	PREPARE_GROW_PHYSICAL_PHASE_START,
	RESUME_PHASE_START,
	RESUME_PHASE_ALLOW_READ_ONLY_MODE,
	RESUME_PHASE_DEDUPE,
	RESUME_PHASE_DEPOT,
	RESUME_PHASE_JOURNAL,
	RESUME_PHASE_BLOCK_MAP,
	RESUME_PHASE_LOGICAL_ZONES,
	RESUME_PHASE_PACKER,
	RESUME_PHASE_FLUSHER,
	RESUME_PHASE_DATA_VIOS,
	RESUME_PHASE_END,
	SUSPEND_PHASE_START,
	SUSPEND_PHASE_PACKER,
	SUSPEND_PHASE_DATA_VIOS,
	SUSPEND_PHASE_DEDUPE,
	SUSPEND_PHASE_FLUSHES,
	SUSPEND_PHASE_LOGICAL_ZONES,
	SUSPEND_PHASE_BLOCK_MAP,
	SUSPEND_PHASE_JOURNAL,
	SUSPEND_PHASE_DEPOT,
	SUSPEND_PHASE_READ_ONLY_WAIT,
	SUSPEND_PHASE_WRITE_SUPER_BLOCK,
	SUSPEND_PHASE_END,
};

static const char * const ADMIN_PHASE_NAMES[] = {
	"GROW_LOGICAL_PHASE_START",
	"GROW_LOGICAL_PHASE_GROW_BLOCK_MAP",
	"GROW_LOGICAL_PHASE_END",
	"GROW_LOGICAL_PHASE_ERROR",
	"GROW_PHYSICAL_PHASE_START",
	"GROW_PHYSICAL_PHASE_COPY_SUMMARY",
	"GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS",
	"GROW_PHYSICAL_PHASE_USE_NEW_SLABS",
	"GROW_PHYSICAL_PHASE_END",
	"GROW_PHYSICAL_PHASE_ERROR",
	"LOAD_PHASE_START",
	"LOAD_PHASE_STATS",
	"LOAD_PHASE_LOAD_DEPOT",
	"LOAD_PHASE_MAKE_DIRTY",
	"LOAD_PHASE_PREPARE_TO_ALLOCATE",
	"LOAD_PHASE_SCRUB_SLABS",
	"LOAD_PHASE_DATA_REDUCTION",
	"LOAD_PHASE_FINISHED",
	"LOAD_PHASE_DRAIN_JOURNAL",
	"LOAD_PHASE_WAIT_FOR_READ_ONLY",
	"PRE_LOAD_PHASE_START",
	"PRE_LOAD_PHASE_LOAD_COMPONENTS",
	"PRE_LOAD_PHASE_END",
	"PREPARE_GROW_PHYSICAL_PHASE_START",
	"RESUME_PHASE_START",
	"RESUME_PHASE_ALLOW_READ_ONLY_MODE",
	"RESUME_PHASE_DEDUPE",
	"RESUME_PHASE_DEPOT",
	"RESUME_PHASE_JOURNAL",
	"RESUME_PHASE_BLOCK_MAP",
	"RESUME_PHASE_LOGICAL_ZONES",
	"RESUME_PHASE_PACKER",
	"RESUME_PHASE_FLUSHER",
	"RESUME_PHASE_DATA_VIOS",
	"RESUME_PHASE_END",
	"SUSPEND_PHASE_START",
	"SUSPEND_PHASE_PACKER",
	"SUSPEND_PHASE_DATA_VIOS",
	"SUSPEND_PHASE_DEDUPE",
	"SUSPEND_PHASE_FLUSHES",
	"SUSPEND_PHASE_LOGICAL_ZONES",
	"SUSPEND_PHASE_BLOCK_MAP",
	"SUSPEND_PHASE_JOURNAL",
	"SUSPEND_PHASE_DEPOT",
	"SUSPEND_PHASE_READ_ONLY_WAIT",
	"SUSPEND_PHASE_WRITE_SUPER_BLOCK",
	"SUSPEND_PHASE_END",
};

#ifndef __KERNEL__
struct registered_thread {
	int dummy;
};

static void uds_register_allocating_thread(struct registered_thread *thread __always_unused,
					   void *context __always_unused)
{
}

static void uds_register_thread_device_id(struct registered_thread *thread __always_unused,
					  unsigned int *instance __always_unused)
{
}

static void uds_unregister_thread_device_id(void)
{
}

static void uds_unregister_allocating_thread(void)
{
}

#endif /* not __KERNEL__ */
static struct vdo *get_vdo_for_target(struct dm_target *ti)
{
	return ((struct device_config *) ti->private)->vdo;
}

#ifdef VDO_INTERNAL
static int check_bio_validity(struct bio *bio)
{
	/* We should never get any other types of bio. */
	bool is_known_type = ((bio_op(bio) == REQ_OP_READ)  ||
			      (bio_op(bio) == REQ_OP_WRITE) ||
			      (bio_op(bio) == REQ_OP_FLUSH) ||
			      (bio_op(bio) == REQ_OP_DISCARD));
	unsigned int known_flags = (REQ_SYNC | REQ_META | REQ_PRIO |
				    REQ_NOMERGE | REQ_IDLE | REQ_FUA |
				    REQ_RAHEAD | REQ_BACKGROUND);
	unsigned int bio_flags = (bio->bi_opf & ~REQ_OP_MASK);
	bool is_empty = (bio->bi_iter.bi_size == 0);
	int result;

	if (!is_known_type) {
		/* XXX Why shouldn't this be assert like the other branches? */
		uds_log_error("Received unexpected bio of type %d", bio_op(bio));
		return -EINVAL;
	}

	/* Is this a flush? It must be empty. */
	if ((bio_op(bio) == REQ_OP_FLUSH) || ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		result = ASSERT(is_empty, "flush bios must be empty");
		if (result != UDS_SUCCESS)
			result = -EINVAL;

		return result;
	}

	/* Is this anything else? It must not be empty. */
	result = ASSERT(!is_empty, "data bios must not be empty");
	if (result != UDS_SUCCESS)
		return -EINVAL;

	/* Is this something other than a discard? Must have size <= 4k. */
	if (bio_op(bio) != REQ_OP_DISCARD) {
		result = ASSERT(bio->bi_iter.bi_size <= VDO_BLOCK_SIZE,
				"data bios must not be more than %d bytes",
				VDO_BLOCK_SIZE);
		if (result != UDS_SUCCESS)
			return -EINVAL;
	}

	/*
	 * Does this have unexpected flags? We expect to never get failfast, integrity, nowait,
	 * cgroup_punt, nounmap, hipri, drv, or swap flags.
	 */
	if ((bio_flags & known_flags) != bio_flags) {
		static DEFINE_RATELIMIT_STATE(unknown_flags_limiter,
					      DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);
		if (__ratelimit(&unknown_flags_limiter))
			uds_log_warning("Bio received with unexpected flags 0x%x (can handle 0x%x)",
					bio_flags, known_flags);
	}

	return 0;
}
#endif /* VDO_INTERNAL */

static int vdo_map_bio(struct dm_target *ti, struct bio *bio)
{
#ifdef VDO_INTERNAL
	int result;
#endif /* VDO_INTERNAL */
	struct vdo *vdo = get_vdo_for_target(ti);
	struct vdo_work_queue *current_work_queue;
	const struct admin_state_code *code = vdo_get_admin_state_code(&vdo->admin.state);

	ASSERT_LOG_ONLY(code->normal, "vdo should not receive bios while in state %s", code->name);

	/* Count all incoming bios. */
	vdo_count_bios(&vdo->stats.bios_in, bio);

#ifdef VDO_INTERNAL
	/* Check for invalid bios. This is too expensive to do except in debug. */
	result = check_bio_validity(bio);
	if (result != 0)
		return result;
#endif /* VDO_INTERNAL */

	/* Handle empty bios.  Empty flush bios are not associated with a vio. */
	if ((bio_op(bio) == REQ_OP_FLUSH) || ((bio->bi_opf & REQ_PREFLUSH) != 0)) {
		vdo_launch_flush(vdo, bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* This could deadlock, */
	current_work_queue = get_current_work_queue();
	BUG_ON((current_work_queue != NULL) &&
	       (vdo == get_work_queue_owner(current_work_queue)->vdo));
	vdo_launch_bio(vdo->data_vio_pool, bio);
	return DM_MAPIO_SUBMITTED;
}

#ifdef __KERNEL__
static void vdo_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct vdo *vdo = get_vdo_for_target(ti);

	limits->logical_block_size = vdo->device_config->logical_block_size;
	limits->physical_block_size = VDO_BLOCK_SIZE;

	/* The minimum io size for random io */
	blk_limits_io_min(limits, VDO_BLOCK_SIZE);
	/* The optimal io size for streamed/sequential io */
	blk_limits_io_opt(limits, VDO_BLOCK_SIZE);

	/*
	 * Sets the maximum discard size that will be passed into VDO. This value comes from a
	 * table line value passed in during dmsetup create.
	 *
	 * The value 1024 is the largest usable value on HD systems.  A 2048 sector discard on a
	 * busy HD system takes 31 seconds.  We should use a value no higher than 1024, which takes
	 * 15 to 16 seconds on a busy HD system.
	 *
	 * But using large values results in 120 second blocked task warnings in /var/log/kern.log.
	 * In order to avoid these warnings, we choose to use the smallest reasonable value.  See
	 * VDO-3062 and VDO-3087.
	 *
	 * The value is displayed in sysfs, and also used by dm-thin to determine whether to pass
	 * down discards. The block layer splits large discards on this boundary when this is set.
	 */
	limits->max_discard_sectors =
		(vdo->device_config->max_discard_blocks * VDO_SECTORS_PER_BLOCK);

	/*
	 * Force discards to not begin or end with a partial block by stating the granularity is
	 * 4k.
	 */
	limits->discard_granularity = VDO_BLOCK_SIZE;
}

static int vdo_iterate_devices(struct dm_target *ti, iterate_devices_callout_fn fn, void *data)
{
	struct device_config *config = get_vdo_for_target(ti)->device_config;

	return fn(ti,
		  config->owned_device,
		  0,
		  config->physical_blocks * VDO_SECTORS_PER_BLOCK,
		  data);
}

/*
 * Status line is:
 *    <device> <operating mode> <in recovery> <index state> <compression state>
 *    <used physical blocks> <total physical blocks>
 */

static void vdo_status(struct dm_target *ti,
		       status_type_t status_type,
		       unsigned int status_flags,
		       char *result,
		       unsigned int maxlen)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct vdo_statistics *stats;
	struct device_config *device_config;
	/* N.B.: The DMEMIT macro uses the variables named "sz", "result", "maxlen". */
	int sz = 0;

	switch (status_type) {
	case STATUSTYPE_INFO:
		/* Report info for dmsetup status */
		mutex_lock(&vdo->stats_mutex);
		vdo_fetch_statistics(vdo, &vdo->stats_buffer);
		stats = &vdo->stats_buffer;

		DMEMIT("/dev/%pg %s %s %s %s %llu %llu",
		       vdo_get_backing_device(vdo),
		       stats->mode,
		       stats->in_recovery_mode ? "recovering" : "-",
		       vdo_get_dedupe_index_state_name(vdo->hash_zones),
		       vdo_get_compressing(vdo) ? "online" : "offline",
		       stats->data_blocks_used + stats->overhead_blocks_used,
		       stats->physical_blocks);
		mutex_unlock(&vdo->stats_mutex);
		break;

	case STATUSTYPE_TABLE:
		/* Report the string actually specified in the beginning. */
		device_config = (struct device_config *) ti->private;
		DMEMIT("%s", device_config->original_string);
		break;

	case STATUSTYPE_IMA:
		/* FIXME: We ought to be more detailed here, but this is what thin does. */
		*result = '\0';
		break;
	}
}

#endif /* __KERNEL__ */
static block_count_t __must_check get_underlying_device_block_count(const struct vdo *vdo)
{
	return i_size_read(vdo_get_backing_device(vdo)->bd_inode) / VDO_BLOCK_SIZE;
}

static int __must_check process_vdo_message_locked(struct vdo *vdo, unsigned int argc, char **argv)
{
	if ((argc == 2) && (strcasecmp(argv[0], "compression") == 0)) {
		if (strcasecmp(argv[1], "on") == 0) {
			vdo_set_compressing(vdo, true);
			return 0;
		}

		if (strcasecmp(argv[1], "off") == 0) {
			vdo_set_compressing(vdo, false);
			return 0;
		}

		uds_log_warning("invalid argument '%s' to dmsetup compression message", argv[1]);
		return -EINVAL;
	}

	uds_log_warning("unrecognized dmsetup message '%s' received", argv[0]);
	return -EINVAL;
}

/*
 * If the message is a dump, just do it. Otherwise, check that no other message is being processed,
 * and only proceed if so.
 * Returns -EBUSY if another message is being processed
 */
static int __must_check process_vdo_message(struct vdo *vdo, unsigned int argc, char **argv)
{
	int result;

	/*
	 * All messages which may be processed in parallel with other messages should be handled
	 * here before the atomic check below. Messages which should be exclusive should be
	 * processed in process_vdo_message_locked().
	 */

	/* Dump messages should always be processed */
	if (strcasecmp(argv[0], "dump") == 0)
		return vdo_dump(vdo, argc, argv, "dmsetup message");

	if (argc == 1) {
		if (strcasecmp(argv[0], "dump-on-shutdown") == 0) {
			vdo->dump_on_shutdown = true;
			return 0;
		}

		/* Index messages should always be processed */
		if ((strcasecmp(argv[0], "index-close") == 0) ||
		    (strcasecmp(argv[0], "index-create") == 0) ||
		    (strcasecmp(argv[0], "index-disable") == 0) ||
		    (strcasecmp(argv[0], "index-enable") == 0))
			return vdo_message_dedupe_index(vdo->hash_zones, argv[0]);
	}

	if (atomic_cmpxchg(&vdo->processing_message, 0, 1) != 0)
		return -EBUSY;

	result = process_vdo_message_locked(vdo, argc, argv);

	/* Pairs with the implicit barrier in cmpxchg just above */
	smp_wmb();
	atomic_set(&vdo->processing_message, 0);
	return result;
}

static int vdo_message(struct dm_target *ti,
		       unsigned int argc,
		       char **argv,
		       char *result_buffer,
		       unsigned int maxlen)
{
	struct registered_thread allocating_thread, instance_thread;
	struct vdo *vdo;
	int result;

	if (argc == 0) {
		uds_log_warning("unspecified dmsetup message");
		return -EINVAL;
	}

	vdo = get_vdo_for_target(ti);
	uds_register_allocating_thread(&allocating_thread, NULL);
	uds_register_thread_device_id(&instance_thread, &vdo->instance);

	/*
	 * Must be done here so we don't map return codes. The code in dm-ioctl expects a 1 for a
	 * return code to look at the buffer and see if it is full or not.
	 */
	if ((argc == 1) && (strcasecmp(argv[0], "stats") == 0)) {
#ifdef __KERNEL__
		vdo_write_stats(vdo, result_buffer, maxlen);
#else /* not __KERNEL__ */
		if (maxlen > 0)
			*result_buffer = '\0';
#endif /* __KERNEL__ */
		result = 1;
	} else {
		result = vdo_map_to_system_error(process_vdo_message(vdo, argc, argv));
	}

	uds_unregister_thread_device_id();
	uds_unregister_allocating_thread();
	return result;
}

#ifdef __KERNEL__
static void configure_target_capabilities(struct dm_target *ti)
{
	ti->discards_supported = 1;
	ti->flush_supported = true;
	ti->num_discard_bios = 1;
	ti->num_flush_bios = 1;

	/*
	 * If this value changes, please make sure to update the value for max_discard_sectors
	 * accordingly.
	 */
	BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);
}

#endif /* __KERNEL__ */
/*
 * Implements vdo_filter_t.
 */
static bool vdo_uses_device(struct vdo *vdo, const void *context)
{
	const struct device_config *config = context;

	return vdo_get_backing_device(vdo)->bd_dev == config->owned_device->bdev->bd_dev;
}

/**
 * get_thread_id_for_phase() - Get the thread id for the current phase of the admin operation in
 *                             progress.
 */
static thread_id_t __must_check
get_thread_id_for_phase(struct vdo *vdo)
{
	const struct thread_config *thread_config = vdo->thread_config;

	switch (vdo->admin.phase) {
	case RESUME_PHASE_PACKER:
	case RESUME_PHASE_FLUSHER:
	case SUSPEND_PHASE_PACKER:
	case SUSPEND_PHASE_FLUSHES:
		return thread_config->packer_thread;

	case RESUME_PHASE_DATA_VIOS:
	case SUSPEND_PHASE_DATA_VIOS:
		return thread_config->cpu_thread;

	case LOAD_PHASE_DRAIN_JOURNAL:
	case RESUME_PHASE_JOURNAL:
	case SUSPEND_PHASE_JOURNAL:
		return thread_config->journal_thread;

	default:
		return thread_config->admin_thread;
	}
}

static struct vdo_completion *prepare_admin_completion(struct vdo *vdo,
						       vdo_action *callback,
						       vdo_action *error_handler)
{
	struct vdo_completion *completion = &vdo->admin.completion;

	/*
	 * We can't use vdo_prepare_completion_for_requeue() here because we don't want to reset
	 * any error in the completion.
	 */
	completion->callback = callback;
	completion->error_handler = error_handler;
	completion->callback_thread_id = get_thread_id_for_phase(vdo);
	completion->requeue = true;
	return completion;
}

/**
 * advance_phase(): Increment the phase of the current admin operation and prepare the admin
 *                  completion to run on the thread for the next phase.
 * @vdo: The on which an admin operation is being performed
 *
 * Return: The current phase
 */
static u32 advance_phase(struct vdo *vdo)
{
	u32 phase = vdo->admin.phase++;

	vdo->admin.completion.callback_thread_id = get_thread_id_for_phase(vdo);
	vdo->admin.completion.requeue = true;
	return phase;
}

/*
 * Perform an administrative operation (load, suspend, grow logical, or grow physical). This method
 * should not be called from vdo threads.
 */
static int perform_admin_operation(struct vdo *vdo,
				   u32 starting_phase,
				   vdo_action *callback,
				   vdo_action *error_handler,
				   const char *type)
{
	int result;
	struct vdo_administrator *admin = &vdo->admin;

	if (atomic_cmpxchg(&admin->busy, 0, 1) != 0)
		return uds_log_error_strerror(VDO_COMPONENT_BUSY,
					      "Can't start %s operation, another operation is already in progress",
					      type);

	admin->phase = starting_phase;
	reinit_completion(&admin->callback_sync);
	vdo_reset_completion(&admin->completion);
	vdo_invoke_completion_callback(prepare_admin_completion(vdo, callback, error_handler));

	/*
	 * Using the "interruptible" interface means that Linux will not log a message when we wait
	 * for more than 120 seconds.
	 */
	while (wait_for_completion_interruptible(&admin->callback_sync) != 0)
		/* * However, if we get a signal in a user-mode process, we could spin... */
		fsleep(1000);

	result = admin->completion.result;
	/* pairs with implicit barrier in cmpxchg above */
	smp_wmb();
	atomic_set(&admin->busy, 0);
	return result;
}

/* Assert that we are operating on the correct thread for the current phase. */
static void assert_admin_phase_thread(struct vdo *vdo, const char *what)
{
	ASSERT_LOG_ONLY(vdo_get_callback_thread_id() == get_thread_id_for_phase(vdo),
			"%s on correct thread for %s",
			what,
			ADMIN_PHASE_NAMES[vdo->admin.phase]);
}

/**
 * finish_operation_callback() - Callback to finish an admin operation.
 * @completion: The admin_completion.
 */
static void finish_operation_callback(struct vdo_completion *completion)
{
	struct vdo_administrator *admin = &completion->vdo->admin;

	vdo_finish_operation(&admin->state, completion->result);
	complete(&admin->callback_sync);
}

/**
 * decode_from_super_block() - Decode the VDO state from the super block and validate that it is
 *                             correct.
 * @vdo: The vdo being loaded.
 *
 * On error from this method, the component states must be destroyed explicitly. If this method
 * returns successfully, the component states must not be destroyed.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check decode_from_super_block(struct vdo *vdo)
{
	const struct device_config *config = vdo->device_config;
	struct super_block_codec *codec = vdo_get_super_block_codec(vdo->super_block);
	int result;

	result = vdo_decode_component_states(codec->component_buffer,
					     vdo->geometry.release_version,
					     &vdo->states);
	if (result != VDO_SUCCESS)
		return result;

	vdo_set_state(vdo, vdo->states.vdo.state);
	vdo->load_state = vdo->states.vdo.state;

	/*
	 * If the device config specifies a larger logical size than was recorded in the super
	 * block, just accept it.
	 */
	if (vdo->states.vdo.config.logical_blocks < config->logical_blocks) {
		uds_log_warning("Growing logical size: a logical size of %llu blocks was specified, but that differs from the %llu blocks configured in the vdo super block",
				(unsigned long long) config->logical_blocks,
				(unsigned long long) vdo->states.vdo.config.logical_blocks);
		vdo->states.vdo.config.logical_blocks = config->logical_blocks;
	}

	result = vdo_validate_component_states(&vdo->states,
					       vdo->geometry.nonce,
					       config->physical_blocks,
					       config->logical_blocks);
	if (result != VDO_SUCCESS)
		return result;

	return vdo_decode_layout(vdo->states.layout, &vdo->layout);
}

/**
 * decode_vdo() - Decode the component data portion of a super block and fill in the corresponding
 *                portions of the vdo being loaded.
 * @vdo: The vdo being loaded.
 *
 * This will also allocate the recovery journal and slab depot. If this method is called with an
 * asynchronous layer (i.e. a thread config which specifies at least one base thread), the block
 * map and packer will be constructed as well.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check decode_vdo(struct vdo *vdo)
{
	block_count_t maximum_age, journal_length;
	const struct thread_config *thread_config = vdo->thread_config;
	int result;

	result = decode_from_super_block(vdo);
	if (result != VDO_SUCCESS) {
		vdo_destroy_component_states(&vdo->states);
		return result;
	}

	maximum_age = vdo_convert_maximum_age(vdo->device_config->block_map_maximum_age);
	journal_length =
		vdo_get_recovery_journal_length(vdo->states.vdo.config.recovery_journal_size);
	if (maximum_age > (journal_length / 2))
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "maximum age: %llu exceeds limit %llu",
					      (unsigned long long) maximum_age,
					      (unsigned long long) (journal_length / 2));

	if (maximum_age == 0)
		return uds_log_error_strerror(VDO_BAD_CONFIGURATION,
					      "maximum age must be greater than 0");

	result = vdo_make_read_only_notifier(vdo_in_read_only_mode(vdo),
					     thread_config,
					     vdo,
					     &vdo->read_only_notifier);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_enable_read_only_entry(vdo);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_recovery_journal(vdo->states.recovery_journal,
					     vdo->states.vdo.nonce,
					     vdo,
					     vdo_get_partition(vdo->layout,
							       VDO_RECOVERY_JOURNAL_PARTITION),
					     vdo->states.vdo.complete_recoveries,
					     vdo->states.vdo.config.recovery_journal_size,
					     vdo->read_only_notifier,
					     thread_config,
					     &vdo->recovery_journal);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_slab_depot(vdo->states.slab_depot,
				       vdo,
				       vdo_get_partition(vdo->layout, VDO_SLAB_SUMMARY_PARTITION),
				       &vdo->depot);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_block_map(vdo->states.block_map,
				      vdo->states.vdo.config.logical_blocks,
				      thread_config,
				      vdo,
				      vdo->read_only_notifier,
				      vdo->recovery_journal,
				      vdo->states.vdo.nonce,
				      vdo->device_config->cache_size,
				      maximum_age,
				      &vdo->block_map);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_make_physical_zones(vdo, &vdo->physical_zones);
	if (result != VDO_SUCCESS)
		return result;

	/* The logical zones depend on the physical zones already existing. */
	result = vdo_make_logical_zones(vdo, &vdo->logical_zones);
	if (result != VDO_SUCCESS)
		return result;

	return vdo_make_hash_zones(vdo, &vdo->hash_zones);
}

/**
 * pre_load_callback() - Callback to initiate a pre-load, registered in vdo_initialize().
 * @completion: The admin completion.
 */
static void pre_load_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case PRE_LOAD_PHASE_START:
		result = vdo_start_operation(&vdo->admin.state, VDO_ADMIN_STATE_PRE_LOADING);
		if (result != VDO_SUCCESS) {
			vdo_continue_completion(completion, result);
			return;
		}

		vdo_load_super_block(vdo,
				     completion,
				     vdo_get_data_region_start(vdo->geometry),
				     &vdo->super_block);
		return;

	case PRE_LOAD_PHASE_LOAD_COMPONENTS:
		vdo_continue_completion(completion, decode_vdo(vdo));
		return;

	case PRE_LOAD_PHASE_END:
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_operation_callback(completion);
}

static void set_device_config(struct dm_target *ti, struct vdo *vdo, struct device_config *config)
{
	vdo_set_device_config(config, vdo);
	ti->private = config;
#ifdef __KERNEL__
	configure_target_capabilities(ti);
#endif /* __KERNEL__ */
}

static int
vdo_initialize(struct dm_target *ti, unsigned int instance, struct device_config *config)
{
	struct vdo *vdo;
	int result;
	u64 block_size = VDO_BLOCK_SIZE;
	u64 logical_size = to_bytes(ti->len);
	block_count_t logical_blocks = logical_size / block_size;

	uds_log_info("loading device '%s'", vdo_get_device_name(ti));
	uds_log_debug("Logical block size     = %llu", (u64) config->logical_block_size);
	uds_log_debug("Logical blocks         = %llu", logical_blocks);
	uds_log_debug("Physical block size    = %llu", (u64) block_size);
	uds_log_debug("Physical blocks        = %llu", config->physical_blocks);
	uds_log_debug("Block map cache blocks = %u", config->cache_size);
	uds_log_debug("Block map maximum age  = %u", config->block_map_maximum_age);
	uds_log_debug("Deduplication          = %s", (config->deduplication ? "on" : "off"));
	uds_log_debug("Compression            = %s", (config->compression ? "on" : "off"));

	vdo = vdo_find_matching(vdo_uses_device, config);
	if (vdo != NULL) {
		uds_log_error("Existing vdo already uses device %s",
			      vdo->device_config->parent_device_name);
		vdo_release_instance(instance);
		ti->error = "Cannot share storage device with already-running VDO";
		return VDO_BAD_CONFIGURATION;
	}

	result = vdo_make(instance, config, &ti->error, &vdo);
	if (result != VDO_SUCCESS) {
		uds_log_error("Could not create VDO device. (VDO error %d, message %s)",
			      result,
			      ti->error);
		vdo_destroy(vdo);
		return result;
	}

	result = perform_admin_operation(vdo,
					 PRE_LOAD_PHASE_START,
					 pre_load_callback,
					 finish_operation_callback,
					 "pre-load");
	if (result != VDO_SUCCESS) {
		ti->error = ((result == VDO_INVALID_ADMIN_STATE) ?
			     "Pre-load is only valid immediately after initialization" :
			     "Cannot load metadata from device");
		uds_log_error("Could not start VDO device. (VDO error %d, message %s)",
			      result,
			      ti->error);
		vdo_destroy(vdo);
		return result;
	}

	set_device_config(ti, vdo, config);
	vdo->device_config = config;
	return VDO_SUCCESS;
}

/* Implements vdo_filter_t. */
static bool __must_check vdo_is_named(struct vdo *vdo, const void *context)
{
	struct dm_target *ti = vdo->device_config->owning_target;
	const char *device_name = vdo_get_device_name(ti);

	return strcmp(device_name, (const char *) context) == 0;
}

static int construct_new_vdo_registered(struct dm_target *ti,
					unsigned int argc,
					char **argv,
					unsigned int instance)
{
	int result;
	struct device_config *config;

	result = vdo_parse_device_config(argc, argv, ti, &config);
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result, "parsing failed: %s", ti->error);
		vdo_release_instance(instance);
		return -EINVAL;
	}

	/* Beyond this point, the instance number will be cleaned up for us if needed */
	result = vdo_initialize(ti, instance, config);
	if (result != VDO_SUCCESS) {
		vdo_free_device_config(config);
		return vdo_map_to_system_error(result);
	}

	return VDO_SUCCESS;
}

static int construct_new_vdo(struct dm_target *ti, unsigned int argc, char **argv)
{
	int result;
	unsigned int instance;
	struct registered_thread instance_thread;

	result = vdo_allocate_instance(&instance);
	if (result != VDO_SUCCESS)
		return -ENOMEM;

	uds_register_thread_device_id(&instance_thread, &instance);
	result = construct_new_vdo_registered(ti, argc, argv, instance);
	uds_unregister_thread_device_id();
	return result;
}

/**
 * check_may_grow_physical() - Callback to check that we're not in recovery mode, used in
 *                             vdo_prepare_to_grow_physical().
 * @completion: The admin completion.
 */
static void check_may_grow_physical(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	assert_admin_phase_thread(vdo, __func__);

	/* These checks can only be done from a vdo thread. */
	if (vdo_is_read_only(vdo->read_only_notifier))
		vdo_set_completion_result(completion, VDO_READ_ONLY);

	if (vdo_in_recovery_mode(vdo))
		vdo_set_completion_result(completion, VDO_RETRY_AFTER_REBUILD);

	finish_operation_callback(completion);
}

static int prepare_to_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size;
	block_count_t current_physical_blocks = vdo->states.vdo.config.physical_blocks;

	uds_log_info("Preparing to resize physical to %llu",
		     (unsigned long long) new_physical_blocks);
	ASSERT_LOG_ONLY((new_physical_blocks > current_physical_blocks),
			"New physical size is larger than current physical size");
	result = perform_admin_operation(vdo,
					 PREPARE_GROW_PHYSICAL_PHASE_START,
					 check_may_grow_physical,
					 finish_operation_callback,
					 "prepare grow-physical");
	if (result != VDO_SUCCESS)
		return result;

	result = prepare_to_vdo_grow_layout(vdo->layout,
					    current_physical_blocks,
					    new_physical_blocks);
	if (result != VDO_SUCCESS)
		return result;

	new_depot_size = vdo_get_next_block_allocator_partition_size(vdo->layout);
	result = vdo_prepare_to_grow_slab_depot(vdo->depot, new_depot_size);
	if (result != VDO_SUCCESS) {
		vdo_finish_layout_growth(vdo->layout);
		return result;
	}

	uds_log_info("Done preparing to resize physical");
	return VDO_SUCCESS;
}

static int prepare_to_modify(struct dm_target *ti, struct device_config *config, struct vdo *vdo)
{
	int result;
	bool may_grow = (vdo_get_admin_state(vdo) != VDO_ADMIN_STATE_PRE_LOADED);

	result = vdo_validate_new_device_config(config, vdo->device_config, may_grow, &ti->error);
	if (result != VDO_SUCCESS)
		return -EINVAL;

	if (config->logical_blocks > vdo->device_config->logical_blocks) {
		block_count_t logical_blocks = vdo->states.vdo.config.logical_blocks;

		uds_log_info("Preparing to resize logical to %llu",
			     (unsigned long long) config->logical_blocks);
		ASSERT_LOG_ONLY((config->logical_blocks > logical_blocks),
				"New logical size is larger than current size");

		result = vdo_prepare_to_grow_block_map(vdo->block_map, config->logical_blocks);
		if (result != VDO_SUCCESS) {
			ti->error = "Device vdo_prepare_to_grow_logical failed";
			return result;
		}

		uds_log_info("Done preparing to resize logical");
	}

	if (config->physical_blocks > vdo->device_config->physical_blocks) {
		result = prepare_to_grow_physical(vdo, config->physical_blocks);
		if (result != VDO_SUCCESS) {
			if (result == VDO_PARAMETER_MISMATCH)
				/*
				 * If we don't trap this case, vdo_map_to_system_error() will remap
				 * it to -EIO, which is misleading and ahistorical.
				 */
				result = -EINVAL;

			if (result == VDO_TOO_MANY_SLABS)
				ti->error = "Device vdo_prepare_to_grow_physical failed (specified physical size too big based on formatted slab size)";
			else
				ti->error = "Device vdo_prepare_to_grow_physical failed";

			return result;
		}
	}

	if (strcmp(config->parent_device_name, vdo->device_config->parent_device_name) != 0) {
		const char *device_name = vdo_get_device_name(config->owning_target);

		uds_log_info("Updating backing device of %s from %s to %s",
			     device_name,
			     vdo->device_config->parent_device_name,
			     config->parent_device_name);
	}

	return VDO_SUCCESS;
}

static int update_existing_vdo(const char *device_name,
			       struct dm_target *ti,
			       unsigned int argc,
			       char **argv,
			       struct vdo *vdo)
{
	int result;
	struct device_config *config;

	result = vdo_parse_device_config(argc, argv, ti, &config);
	if (result != VDO_SUCCESS)
		return -EINVAL;

	uds_log_info("preparing to modify device '%s'", device_name);
	result = prepare_to_modify(ti, config, vdo);
	if (result != VDO_SUCCESS) {
		vdo_free_device_config(config);
		return vdo_map_to_system_error(result);
	}

	set_device_config(ti, vdo, config);
	return VDO_SUCCESS;
}

static int vdo_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	int result;
	struct registered_thread allocating_thread, instance_thread;
	const char *device_name;
	struct vdo *vdo;

	uds_register_allocating_thread(&allocating_thread, NULL);
	device_name = vdo_get_device_name(ti);
	vdo = vdo_find_matching(vdo_is_named, (const void *) device_name);
	if (vdo == NULL) {
		result = construct_new_vdo(ti, argc, argv);
	} else {
		uds_register_thread_device_id(&instance_thread, &vdo->instance);
		result = update_existing_vdo(device_name, ti, argc, argv, vdo);
		uds_unregister_thread_device_id();
	}

	uds_unregister_allocating_thread();
	return result;
}

static void vdo_dtr(struct dm_target *ti)
{
	struct device_config *config = ti->private;
	struct vdo *vdo = config->vdo;

	vdo_set_device_config(config, NULL);
	if (list_empty(&vdo->device_config_list)) {
		const char *device_name;

		/* This was the last config referencing the VDO. Free it. */
		unsigned int instance = vdo->instance;
		struct registered_thread allocating_thread, instance_thread;

		uds_register_thread_device_id(&instance_thread, &instance);
		uds_register_allocating_thread(&allocating_thread, NULL);

		device_name = vdo_get_device_name(ti);
		uds_log_info("stopping device '%s'", device_name);
		if (vdo->dump_on_shutdown)
			vdo_dump_all(vdo, "device shutdown");

		vdo_destroy(UDS_FORGET(vdo));
		uds_log_info("device '%s' stopped", device_name);
		uds_unregister_thread_device_id();
		uds_unregister_allocating_thread();
	} else if (config == vdo->device_config) {
		/*
		 * The VDO still references this config. Give it a reference to a config that isn't
		 * being destroyed.
		 */
		vdo->device_config = list_first_entry(&vdo->device_config_list,
						      struct device_config,
						      config_list);
	}

	vdo_free_device_config(config);
	ti->private = NULL;
}

static void vdo_presuspend(struct dm_target *ti)
{
	get_vdo_for_target(ti)->suspend_type =
		(dm_noflush_suspending(ti) ? VDO_ADMIN_STATE_SUSPENDING : VDO_ADMIN_STATE_SAVING);
}

/**
 * write_super_block_for_suspend() - Update the VDO state and save the super block.
 * @completion: The admin completion
 */
static void write_super_block_for_suspend(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	switch (vdo_get_state(vdo)) {
	case VDO_DIRTY:
	case VDO_NEW:
		vdo_set_state(vdo, VDO_CLEAN);
		break;

	case VDO_CLEAN:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		break;

	case VDO_REPLAYING:
	default:
		vdo_continue_completion(completion, UDS_BAD_STATE);
		return;
	}

	vdo_save_components(vdo, completion);
}

/**
 * suspend_callback() - Callback to initiate a suspend, registered in vdo_postsuspend().
 * @completion: The sub-task completion.
 */
static void suspend_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	struct admin_state *state = &vdo->admin.state;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case SUSPEND_PHASE_START:
		if (vdo_get_admin_state_code(state)->quiescent)
			/* Already suspended */
			break;

		vdo_continue_completion(completion, vdo_start_operation(state, vdo->suspend_type));
		return;

	case SUSPEND_PHASE_PACKER:
		/*
		 * If the VDO was already resumed from a prior suspend while read-only, some of the
		 * components may not have been resumed. By setting a read-only error here, we
		 * guarantee that the result of this suspend will be VDO_READ_ONLY and not
		 * VDO_INVALID_ADMIN_STATE in that case.
		 */
		if (vdo_in_read_only_mode(vdo))
			vdo_set_completion_result(completion, VDO_READ_ONLY);

		vdo_drain_packer(vdo->packer, completion);
		return;

	case SUSPEND_PHASE_DATA_VIOS:
		drain_data_vio_pool(vdo->data_vio_pool, completion);
		return;

	case SUSPEND_PHASE_DEDUPE:
		vdo_drain_hash_zones(vdo->hash_zones, completion);
		return;

	case SUSPEND_PHASE_FLUSHES:
		vdo_drain_flusher(vdo->flusher, completion);
		return;

	case SUSPEND_PHASE_LOGICAL_ZONES:
		/*
		 * Attempt to flush all I/O before completing post suspend work. We believe a
		 * suspended device is expected to have persisted all data written before the
		 * suspend, even if it hasn't been flushed yet.
		 */
		result = vdo_synchronous_flush(vdo);
		if (result != VDO_SUCCESS)
			vdo_enter_read_only_mode(vdo->read_only_notifier, result);

		vdo_drain_logical_zones(vdo->logical_zones,
					vdo_get_admin_state_code(state),
					completion);
		return;

	case SUSPEND_PHASE_BLOCK_MAP:
		vdo_drain_block_map(vdo->block_map,
				    vdo_get_admin_state_code(state),
				    completion);
		return;

	case SUSPEND_PHASE_JOURNAL:
		vdo_drain_recovery_journal(vdo->recovery_journal,
					   vdo_get_admin_state_code(state),
					   completion);
		return;

	case SUSPEND_PHASE_DEPOT:
		vdo_drain_slab_depot(vdo->depot,
				     vdo_get_admin_state_code(state),
				     completion);
		return;

	case SUSPEND_PHASE_READ_ONLY_WAIT:
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier, completion);
		return;

	case SUSPEND_PHASE_WRITE_SUPER_BLOCK:
		if (vdo_is_state_suspending(state) || (completion->result != VDO_SUCCESS))
			/* If we didn't save the VDO or there was an error, we're done. */
			break;

		write_super_block_for_suspend(completion);
		return;

	case SUSPEND_PHASE_END:
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_operation_callback(completion);
}

#ifdef INTERNAL
extern int suspend_result;
#endif /* INTERNAL */
static void vdo_postsuspend(struct dm_target *ti)
{
	struct vdo *vdo = get_vdo_for_target(ti);
	struct registered_thread instance_thread;
	const char *device_name;
	int result;

	uds_register_thread_device_id(&instance_thread, &vdo->instance);
	device_name = vdo_get_device_name(vdo->device_config->owning_target);
	uds_log_info("suspending device '%s'", device_name);

	/*
	 * It's important to note any error here does not actually stop device-mapper from
	 * suspending the device. All this work is done post suspend.
	 */
	result = perform_admin_operation(vdo,
					 SUSPEND_PHASE_START,
					 suspend_callback,
					 suspend_callback,
					 "suspend");
#ifdef INTERNAL
	suspend_result = result;
#endif /* INTERNAL */

	if ((result == VDO_SUCCESS) || (result == VDO_READ_ONLY)) {
		/*
		 * Treat VDO_READ_ONLY as a success since a read-only suspension still leaves the
		 * VDO suspended.
		 */
		uds_log_info("device '%s' suspended", device_name);
	} else if (result == VDO_INVALID_ADMIN_STATE) {
		uds_log_error("Suspend invoked while in unexpected state: %s",
			      vdo_get_admin_state(vdo)->name);
	} else {
		uds_log_error_strerror(result, "Suspend of device '%s' failed", device_name);
	}

	uds_unregister_thread_device_id();
}

#ifndef __KERNEL__
/*
 * This is literally the least we can do to for unit tests which don't yet try to simulate or test
 * sysfs.
 */
static void vdo_pool_release(struct kobject *directory)
{
	ASSERT_LOG_ONLY((atomic_read(&(directory->refcount)) == 0),
			"kobject being released has no references");
	struct vdo *vdo = container_of(directory, struct vdo, vdo_directory);

	UDS_FREE(vdo);
}

struct kobj_type vdo_directory_type = {
	.release = vdo_pool_release,
	.sysfs_ops = NULL,
	.default_groups = NULL,
};

#endif /* not __KERNEL__ */
/**
 * was_new() - Check whether the vdo was new when it was loaded.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo was new.
 */
static bool was_new(const struct vdo *vdo)
{
	return (vdo->load_state == VDO_NEW);
}

/**
 * requires_repair() - Check whether a vdo requires recovery or rebuild.
 * @vdo: The vdo to query.
 *
 * Return: true if the vdo must be repaired.
 */
static bool __must_check requires_repair(const struct vdo *vdo)
{
	switch (vdo_get_state(vdo)) {
	case VDO_DIRTY:
	case VDO_FORCE_REBUILD:
	case VDO_REPLAYING:
	case VDO_REBUILD_FOR_UPGRADE:
		return true;

	default:
		return false;
	}
}

/**
 * get_load_type() - Determine how the slab depot was loaded.
 * @vdo: The vdo.
 *
 * Return: How the depot was loaded.
 */
static enum slab_depot_load_type get_load_type(struct vdo *vdo)
{
	if (vdo_state_requires_read_only_rebuild(vdo->load_state))
		return VDO_SLAB_DEPOT_REBUILD_LOAD;

	if (vdo_state_requires_recovery(vdo->load_state))
		return VDO_SLAB_DEPOT_RECOVERY_LOAD;

	return VDO_SLAB_DEPOT_NORMAL_LOAD;
}

/**
 * vdo_initialize_kobjects() - Initialize the vdo sysfs directory.
 * @vdo: The vdo being initialized.
 *
 * Return: VDO_SUCCESS or an error code.
 */
static int vdo_initialize_kobjects(struct vdo *vdo)
{
	int result;
	struct dm_target *target = vdo->device_config->owning_target;
	struct mapped_device *md = dm_table_get_md(target->table);

	kobject_init(&vdo->vdo_directory, &vdo_directory_type);
	vdo->sysfs_added = true;
	result = kobject_add(&vdo->vdo_directory, &disk_to_dev(dm_disk(md))->kobj, "vdo");
	if (result != 0)
		return VDO_CANT_ADD_SYSFS_NODE;

#ifdef VDO_INTERNAL
	vdo_initialize_histograms(&vdo->vdo_directory, &vdo->histograms);
#endif /* VDO_INTERNAL */
	result = vdo_add_dedupe_index_sysfs(vdo->hash_zones);
	if (result != 0)
		return VDO_CANT_ADD_SYSFS_NODE;

	return vdo_add_sysfs_stats_dir(vdo);
}

/**
 * load_callback() - Callback to do the destructive parts of loading a VDO.
 * @completion: The sub-task completion.
 */
static void load_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case LOAD_PHASE_START:
		result = vdo_start_operation(&vdo->admin.state, VDO_ADMIN_STATE_LOADING);
		if (result != VDO_SUCCESS) {
			vdo_continue_completion(completion, result);
			return;
		}

		/* Prepare the recovery journal for new entries. */
		vdo_open_recovery_journal(vdo->recovery_journal, vdo->depot, vdo->block_map);
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier, completion);
		return;

	case LOAD_PHASE_STATS:
		vdo_continue_completion(completion, vdo_initialize_kobjects(vdo));
		return;

	case LOAD_PHASE_LOAD_DEPOT:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			/*
			 * In read-only mode we don't use the allocator and it may not even be
			 * readable, so don't bother trying to load it.
			 */
			vdo_set_completion_result(completion, VDO_READ_ONLY);
			break;
		}

		if (requires_repair(vdo)) {
			vdo_repair(completion);
			return;
		}

		vdo_load_slab_depot(vdo->depot,
				    (was_new(vdo) ?
				     VDO_ADMIN_STATE_FORMATTING :
				     VDO_ADMIN_STATE_LOADING),
				    completion,
				    NULL);
		return;

	case LOAD_PHASE_MAKE_DIRTY:
		vdo_set_state(vdo, VDO_DIRTY);
		vdo_save_components(vdo, completion);
		return;

	case LOAD_PHASE_PREPARE_TO_ALLOCATE:
		vdo_initialize_block_map_from_journal(vdo->block_map, vdo->recovery_journal);
		vdo_prepare_slab_depot_to_allocate(vdo->depot, get_load_type(vdo), completion);
		return;

	case LOAD_PHASE_SCRUB_SLABS:
		if (vdo_state_requires_recovery(vdo->load_state))
			vdo_enter_recovery_mode(vdo);

		vdo_scrub_all_unrecovered_slabs(vdo->depot, completion);
		return;

	case LOAD_PHASE_DATA_REDUCTION:
		WRITE_ONCE(vdo->compressing, vdo->device_config->compression);
		if (vdo->device_config->deduplication)
			/*
			 * Don't try to load or rebuild the index first (and log scary error
			 * messages) if this is known to be a newly-formatted volume.
			 */
			vdo_start_dedupe_index(vdo->hash_zones, was_new(vdo));

		vdo->allocations_allowed = false;
		fallthrough;

	case LOAD_PHASE_FINISHED:
		break;

	case LOAD_PHASE_DRAIN_JOURNAL:
		vdo_drain_recovery_journal(vdo->recovery_journal,
					   VDO_ADMIN_STATE_SAVING,
					   completion);
		return;

	case LOAD_PHASE_WAIT_FOR_READ_ONLY:
		/* Avoid an infinite loop */
		completion->error_handler = NULL;
		vdo->admin.phase = LOAD_PHASE_FINISHED;
		vdo_wait_until_not_entering_read_only_mode(vdo->read_only_notifier, completion);
		return;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_operation_callback(completion);
}

/**
 * handle_load_error() - Handle an error during the load operation.
 * @completion: The admin completion.
 *
 * If at all possible, brings the vdo online in read-only mode. This handler is registered in
 * vdo_preresume_registered().
 */
static void handle_load_error(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	if (vdo_get_callback_thread_id() != vdo->thread_config->admin_thread) {
		completion->callback_thread_id = vdo->thread_config->admin_thread;
		vdo_invoke_completion_callback(completion);
		return;
	}

	if (vdo_state_requires_read_only_rebuild(vdo->load_state) &&
	    (vdo->admin.phase == LOAD_PHASE_MAKE_DIRTY)) {
		uds_log_error_strerror(completion->result, "aborting load");
		vdo->admin.phase = LOAD_PHASE_DRAIN_JOURNAL;
		load_callback(UDS_FORGET(completion));
		return;
	}

	uds_log_error_strerror(completion->result, "Entering read-only mode due to load error");
	vdo->admin.phase = LOAD_PHASE_WAIT_FOR_READ_ONLY;
	vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
	completion->result = VDO_READ_ONLY;
	load_callback(completion);
}

/**
 * write_super_block_for_resume() - Update the VDO state and save the super block.
 * @completion: The admin completion
 */
static void write_super_block_for_resume(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	switch (vdo_get_state(vdo)) {
	case VDO_CLEAN:
	case VDO_NEW:
		vdo_set_state(vdo, VDO_DIRTY);
		vdo_save_components(vdo, completion);
		return;

	case VDO_DIRTY:
	case VDO_READ_ONLY_MODE:
	case VDO_FORCE_REBUILD:
	case VDO_RECOVERING:
	case VDO_REBUILD_FOR_UPGRADE:
		/* No need to write the super block in these cases */
		vdo_invoke_completion_callback(completion);
		return;

	case VDO_REPLAYING:
	default:
		vdo_continue_completion(completion, UDS_BAD_STATE);
	}
}

/**
 * resume_callback() - Callback to resume a VDO.
 * @completion: The admin completion.
 */
static void resume_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case RESUME_PHASE_START:
		result = vdo_start_operation(&vdo->admin.state, VDO_ADMIN_STATE_RESUMING);
		if (result != VDO_SUCCESS) {
			vdo_continue_completion(completion, result);
			return;
		}

		write_super_block_for_resume(completion);
		return;

	case RESUME_PHASE_ALLOW_READ_ONLY_MODE:
		vdo_allow_read_only_mode_entry(vdo->read_only_notifier, completion);
		return;

	case RESUME_PHASE_DEDUPE:
		vdo_resume_hash_zones(vdo->hash_zones, completion);
		return;

	case RESUME_PHASE_DEPOT:
		vdo_resume_slab_depot(vdo->depot, completion);
		return;

	case RESUME_PHASE_JOURNAL:
		vdo_resume_recovery_journal(vdo->recovery_journal, completion);
		return;

	case RESUME_PHASE_BLOCK_MAP:
		vdo_resume_block_map(vdo->block_map, completion);
		return;

	case RESUME_PHASE_LOGICAL_ZONES:
		vdo_resume_logical_zones(vdo->logical_zones, completion);
		return;

	case RESUME_PHASE_PACKER:
	{
		bool was_enabled = vdo_get_compressing(vdo);
		bool enable = vdo->device_config->compression;

		if (enable != was_enabled)
			WRITE_ONCE(vdo->compressing, enable);
		uds_log_info("compression is %s", (enable ? "enabled" : "disabled"));

		vdo_resume_packer(vdo->packer, completion);
		return;
	}

	case RESUME_PHASE_FLUSHER:
		vdo_resume_flusher(vdo->flusher, completion);
		return;

	case RESUME_PHASE_DATA_VIOS:
		resume_data_vio_pool(vdo->data_vio_pool, completion);
		return;

	case RESUME_PHASE_END:
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_operation_callback(completion);
}

/**
 * grow_logical_callback() - Callback to initiate a grow logical.
 * @completion: The admin completion.
 *
 * Registered in perform_grow_logical().
 */
static void grow_logical_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case GROW_LOGICAL_PHASE_START:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			uds_log_error_strerror(VDO_READ_ONLY,
					       "Can't grow logical size of a read-only VDO");
			vdo_set_completion_result(completion, VDO_READ_ONLY);
			break;
		}

		result = vdo_start_operation(&vdo->admin.state,
					     VDO_ADMIN_STATE_SUSPENDED_OPERATION);
		if (result != VDO_SUCCESS) {
			vdo_continue_completion(completion, result);
			return;
		}

		vdo->states.vdo.config.logical_blocks = vdo->block_map->next_entry_count;
		vdo_save_components(vdo, completion);
		return;

	case GROW_LOGICAL_PHASE_GROW_BLOCK_MAP:
		vdo_grow_block_map(vdo->block_map, completion);
		return;

	case GROW_LOGICAL_PHASE_END:
		break;

	case GROW_LOGICAL_PHASE_ERROR:
		vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	finish_operation_callback(completion);
}

/**
 * handle_logical_growth_error() - Handle an error during the grow physical process.
 * @completion: The admin completion.
 */
static void handle_logical_growth_error(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;

	if (vdo->admin.phase == GROW_LOGICAL_PHASE_GROW_BLOCK_MAP) {
		/*
		 * We've failed to write the new size in the super block, so set our in memory
		 * config back to the old size.
		 */
		vdo->states.vdo.config.logical_blocks = vdo->block_map->entry_count;
		vdo_abandon_block_map_growth(vdo->block_map);
	}

	vdo->admin.phase = GROW_LOGICAL_PHASE_ERROR;
	grow_logical_callback(completion);
}

/**
 * perform_grow_logical() - Grow the logical size of the vdo.
 * @vdo: The vdo to grow.
 * @new_logical_blocks: The size to which the vdo should be grown.
 *
 * Context: This method may only be called when the vdo has been suspended and must not be called
 * from a base thread.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int perform_grow_logical(struct vdo *vdo, block_count_t new_logical_blocks)
{
	int result;

	if (vdo->device_config->logical_blocks == new_logical_blocks) {
		/*
		 * A table was loaded for which we prepared to grow, but a table without that
		 * growth was what we are resuming with.
		 */
		vdo_abandon_block_map_growth(vdo->block_map);
		return VDO_SUCCESS;
	}

	uds_log_info("Resizing logical to %llu", (unsigned long long) new_logical_blocks);
	if (vdo->block_map->next_entry_count != new_logical_blocks)
		return VDO_PARAMETER_MISMATCH;

	result = perform_admin_operation(vdo,
					 GROW_LOGICAL_PHASE_START,
					 grow_logical_callback,
					 handle_logical_growth_error,
					 "grow logical");
	if (result != VDO_SUCCESS)
		return result;

	uds_log_info("Logical blocks now %llu", (unsigned long long) new_logical_blocks);
	return VDO_SUCCESS;
}

/**
 * grow_physical_callback() - Callback to initiate a grow physical.
 * @completion: The admin completion.
 *
 * Registered in perform_grow_physical().
 */
static void grow_physical_callback(struct vdo_completion *completion)
{
	struct vdo *vdo = completion->vdo;
	int result;

	assert_admin_phase_thread(vdo, __func__);

	switch (advance_phase(vdo)) {
	case GROW_PHYSICAL_PHASE_START:
		if (vdo_is_read_only(vdo->read_only_notifier)) {
			uds_log_error_strerror(VDO_READ_ONLY,
					       "Can't grow physical size of a read-only VDO");
			vdo_set_completion_result(completion, VDO_READ_ONLY);
			break;
		}

		result = vdo_start_operation(&vdo->admin.state,
					     VDO_ADMIN_STATE_SUSPENDED_OPERATION);
		if (result != VDO_SUCCESS) {
			vdo_continue_completion(completion, result);
			return;
		}

		/* Copy the journal into the new layout. */
		vdo_copy_layout_partition(vdo->layout, VDO_RECOVERY_JOURNAL_PARTITION, completion);
		return;

	case GROW_PHYSICAL_PHASE_COPY_SUMMARY:
		vdo_copy_layout_partition(vdo->layout, VDO_SLAB_SUMMARY_PARTITION, completion);
		return;

	case GROW_PHYSICAL_PHASE_UPDATE_COMPONENTS:
		vdo->states.vdo.config.physical_blocks = vdo_grow_layout(vdo->layout);
		vdo_update_slab_depot_size(vdo->depot);
		vdo_save_components(vdo, completion);
		return;

	case GROW_PHYSICAL_PHASE_USE_NEW_SLABS:
		vdo_use_new_slabs(vdo->depot, completion);
		return;

	case GROW_PHYSICAL_PHASE_END:
		vdo_set_slab_summary_origin(vdo->depot->slab_summary,
					    vdo_get_partition(vdo->layout,
							      VDO_SLAB_SUMMARY_PARTITION));
		vdo_set_recovery_journal_partition(vdo->recovery_journal,
						   vdo_get_partition(vdo->layout,
								     VDO_RECOVERY_JOURNAL_PARTITION));
		break;

	case GROW_PHYSICAL_PHASE_ERROR:
		vdo_enter_read_only_mode(vdo->read_only_notifier, completion->result);
		break;

	default:
		vdo_set_completion_result(completion, UDS_BAD_STATE);
	}

	vdo_finish_layout_growth(vdo->layout);
	finish_operation_callback(completion);
}

/**
 * handle_physical_growth_error() - Handle an error during the grow physical process.
 * @completion: The sub-task completion.
 */
static void handle_physical_growth_error(struct vdo_completion *completion)
{
	completion->vdo->admin.phase = GROW_PHYSICAL_PHASE_ERROR;
	grow_physical_callback(completion);
}

/**
 * perform_grow_physical() - Grow the physical size of the vdo.
 * @vdo: The vdo to resize.
 * @new_physical_blocks: The new physical size in blocks.
 *
 * Context: This method may only be called when the vdo has been suspended and must not be called
 * from a base thread.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int perform_grow_physical(struct vdo *vdo, block_count_t new_physical_blocks)
{
	int result;
	block_count_t new_depot_size, prepared_depot_size;
	block_count_t old_physical_blocks = vdo->states.vdo.config.physical_blocks;

	/* Skip any noop grows. */
	if (old_physical_blocks == new_physical_blocks)
		return VDO_SUCCESS;

	if (new_physical_blocks != vdo_get_next_layout_size(vdo->layout)) {
		/*
		 * Either the VDO isn't prepared to grow, or it was prepared to grow to a different
		 * size. Doing this check here relies on the fact that the call to this method is
		 * done under the dmsetup message lock.
		 */
		vdo_finish_layout_growth(vdo->layout);
		vdo_abandon_new_slabs(vdo->depot);
		return VDO_PARAMETER_MISMATCH;
	}

	/* Validate that we are prepared to grow appropriately. */
	new_depot_size = vdo_get_next_block_allocator_partition_size(vdo->layout);
	prepared_depot_size = (vdo->depot->new_slabs == NULL) ? 0 : vdo->depot->new_size;
	if (prepared_depot_size != new_depot_size)
		return VDO_PARAMETER_MISMATCH;

	result = perform_admin_operation(vdo,
					 GROW_PHYSICAL_PHASE_START,
					 grow_physical_callback,
					 handle_physical_growth_error,
					 "grow physical");
	if (result != VDO_SUCCESS)
		return result;

	uds_log_info("Physical block count was %llu, now %llu",
		     (unsigned long long) old_physical_blocks,
		     (unsigned long long) new_physical_blocks);
	return VDO_SUCCESS;
}

/**
 * apply_new_vdo_configuration() - Attempt to make any configuration changes from the table being
 *                                 resumed.
 * @vdo: The vdo being resumed.
 * @config: The new device configuration derived from the table with which the vdo is being
 *          resumed.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check apply_new_vdo_configuration(struct vdo *vdo, struct device_config *config)
{
	int result;

	result = perform_grow_logical(vdo, config->logical_blocks);
	if (result != VDO_SUCCESS) {
		uds_log_error("grow logical operation failed, result = %d", result);
		return result;
	}

	result = perform_grow_physical(vdo, config->physical_blocks);
	if (result != VDO_SUCCESS)
		uds_log_error("resize operation failed, result = %d", result);

	return result;
}

#ifdef INTERNAL
extern int resume_result;

#endif /* INTERNAL */
static int vdo_preresume_registered(struct dm_target *ti, struct vdo *vdo)
{
	struct device_config *config = ti->private;
	const char *device_name = vdo_get_device_name(ti);
	block_count_t backing_blocks;
	int result;

	backing_blocks = get_underlying_device_block_count(vdo);
	if (backing_blocks < config->physical_blocks) {
		/* FIXME: can this still happen? */
		uds_log_error("resume of device '%s' failed: backing device has %llu blocks but VDO physical size is %llu blocks",
			      device_name,
			      (unsigned long long) backing_blocks,
			      (unsigned long long) config->physical_blocks);
		return -EINVAL;
	}

	if (vdo_get_admin_state(vdo) == VDO_ADMIN_STATE_PRE_LOADED) {
		uds_log_info("starting device '%s'", device_name);
		result = perform_admin_operation(vdo,
						 LOAD_PHASE_START,
						 load_callback,
						 handle_load_error,
						 "load");
		if ((result != VDO_SUCCESS) && (result != VDO_READ_ONLY)) {
			/*
			 * Something has gone very wrong. Make sure everything has drained and
			 * leave the device in an unresumable state.
			 */
			uds_log_error_strerror(result,
					       "Start failed, could not load VDO metadata");
			vdo->suspend_type = VDO_ADMIN_STATE_STOPPING;
			perform_admin_operation(vdo,
						SUSPEND_PHASE_START,
						suspend_callback,
						suspend_callback,
						"suspend");
			return result;
		}

		/* Even if the VDO is read-only, it is now able to handle read requests. */
		uds_log_info("device '%s' started", device_name);
	}

	uds_log_info("resuming device '%s'", device_name);

	/* If this fails, the VDO was not in a state to be resumed. This should never happen. */
	result = apply_new_vdo_configuration(vdo, config);
#ifdef INTERNAL
	resume_result = result;
#endif /* INTERNAL */
	BUG_ON(result == VDO_INVALID_ADMIN_STATE);

	/*
	 * Now that we've tried to modify the vdo, the new config *is* the config, whether the
	 * modifications worked or not.
	 */
	vdo->device_config = config;

	/*
	 * Any error here is highly unexpected and the state of the vdo is questionable, so we mark
	 * it read-only in memory. Because we are suspended, the read-only state will not be
	 * written to disk.
	 */
	if (result != VDO_SUCCESS) {
		uds_log_error_strerror(result,
				       "Commit of modifications to device '%s' failed",
				       device_name);
		vdo_enter_read_only_mode(vdo->read_only_notifier, result);
		return result;
	}

	if (vdo_get_admin_state(vdo)->normal)
		/* The VDO was just started, so we don't need to resume it. */
		return VDO_SUCCESS;

	result = perform_admin_operation(vdo,
					 RESUME_PHASE_START,
					 resume_callback,
					 resume_callback,
					 "resume");
#ifdef INTERNAL
	resume_result = result;
#endif /* INTERNAL */
	BUG_ON(result == VDO_INVALID_ADMIN_STATE);
	if (result == VDO_READ_ONLY)
		/* Even if the vdo is read-only, it has still resumed. */
		result = VDO_SUCCESS;

	if (result != VDO_SUCCESS)
		uds_log_error("resume of device '%s' failed with error: %d", device_name, result);

	return result;
}

static int vdo_preresume(struct dm_target *ti)
{
	struct registered_thread instance_thread;
	struct vdo *vdo = get_vdo_for_target(ti);
	int result;

	uds_register_thread_device_id(&instance_thread, &vdo->instance);
	result = vdo_preresume_registered(ti, vdo);
	if ((result == VDO_PARAMETER_MISMATCH) || (result == VDO_INVALID_ADMIN_STATE))
		result = -EINVAL;
	uds_unregister_thread_device_id();
	return vdo_map_to_system_error(result);
}

static void vdo_resume(struct dm_target *ti)
{
	struct registered_thread instance_thread;

	uds_register_thread_device_id(&instance_thread, &get_vdo_for_target(ti)->instance);
	uds_log_info("device '%s' resumed", vdo_get_device_name(ti));
	uds_unregister_thread_device_id();
}

/*
 * If anything changes that affects how user tools will interact with vdo, update the version
 * number and make sure documentation about the change is complete so tools can properly update
 * their management code.
 */
static struct target_type vdo_target_bio = {
	.features = DM_TARGET_SINGLETON,
	.name = "vdo",
	.version = { 8, 2, 0 },
#ifdef __KERNEL__
	.module = THIS_MODULE,
#endif /* __KERNEL__ */
	.ctr = vdo_ctr,
	.dtr = vdo_dtr,
#ifdef __KERNEL__
	.io_hints = vdo_io_hints,
	.iterate_devices = vdo_iterate_devices,
#endif /* __KERNEL__ */
	.map = vdo_map_bio,
	.message = vdo_message,
#ifdef __KERNEL__
	.status = vdo_status,
#endif /* __KERNEL__ */
	.presuspend = vdo_presuspend,
	.postsuspend = vdo_postsuspend,
	.preresume = vdo_preresume,
	.resume = vdo_resume,
};

static bool dm_registered;

static void vdo_module_destroy(void)
{
	uds_log_debug("unloading");

	if (dm_registered)
		dm_unregister_target(&vdo_target_bio);

	vdo_clean_up_instance_number_tracking();

	uds_log_info("unloaded version %s", CURRENT_VERSION);
}

static int __init vdo_init(void)
{
	int result = 0;

#ifdef __KERNEL__
	/*
	 * UDS module level initialization must be done first, as VDO initialization depends on it
	 */
	uds_initialize_thread_device_registry();
	uds_memory_init();
	uds_init_sysfs();
#endif /* __KERNEL__ */

	vdo_initialize_device_registry_once();
	uds_log_info("loaded version %s", CURRENT_VERSION);

	/* Add VDO errors to the already existing set of errors in UDS. */
	result = vdo_register_status_codes();
	if (result != UDS_SUCCESS) {
		uds_log_error("vdo_register_status_codes failed %d", result);
		vdo_module_destroy();
		return result;
	}

	result = dm_register_target(&vdo_target_bio);
	if (result < 0) {
		uds_log_error("dm_register_target failed %d", result);
		vdo_module_destroy();
		return result;
	}
	dm_registered = true;

	vdo_initialize_instance_number_tracking();

	return result;
}

static void __exit vdo_exit(void)
{
	vdo_module_destroy();
	/*
	 * UDS module level exit processing must be done after all VDO module exit processing is
	 * complete.
	 */
#ifdef __KERNEL__
	uds_put_sysfs();
	uds_memory_exit();
#endif /* __KERNEL__ */
}

module_init(vdo_init);
module_exit(vdo_exit);

#ifdef __KERNEL__
MODULE_DESCRIPTION(DM_NAME " target for transparent deduplication");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
#endif /* __KERNEL__ */
