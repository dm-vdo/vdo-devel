/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DEDUPE_H
#define DEDUPE_H

#include <linux/list.h>

#include "uds.h"

#include "kernel-types.h"
#include "statistics.h"
#include "types.h"

int __must_check
vdo_make_hash_zones(struct vdo *vdo, struct hash_zones **zones_ptr);

void vdo_free_hash_zones(struct hash_zones *zones);

thread_id_t __must_check
vdo_get_hash_zone_thread_id(const struct hash_zone *zone);

void vdo_get_hash_zone_statistics(struct hash_zones *zones,
				  struct hash_lock_statistics *tally);

struct hash_zone * __must_check
vdo_select_hash_zone(struct hash_zones *zones,
		     const struct uds_chunk_name *name);

int __must_check
vdo_acquire_lock_from_hash_zone(struct hash_zone *zone,
				const struct uds_chunk_name *hash,
				struct hash_lock *replace_lock,
				struct hash_lock **lock_ptr);

void vdo_return_lock_to_hash_zone(struct hash_zone *zone,
				  struct hash_lock *lock);

void vdo_bump_hash_zone_valid_advice_count(struct hash_zone *zone);

void vdo_bump_hash_zone_stale_advice_count(struct hash_zone *zone);

void vdo_bump_hash_zone_data_match_count(struct hash_zone *zone);

void vdo_bump_hash_zone_collision_count(struct hash_zone *zone);

void vdo_dump_hash_zones(struct hash_zones *zones);

#endif /* DEDUPE_H */
