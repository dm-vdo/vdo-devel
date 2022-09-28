/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H

#include "types.h"

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
struct vio;
struct vio_pool;

#endif /* KERNEL_TYPES_H */
