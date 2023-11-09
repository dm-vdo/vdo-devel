/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_HASH_MAP_H
#define VDO_HASH_MAP_H

#include <linux/compiler.h>
#include <linux/types.h>

#include "hash-map.h"

/*
 * A vdo_hash_map with HASH_MAP_TYPE_INT associates pointers (void *) with integer keys (u64).
 * NULL pointer values are not supported.
 *
 * A vdo_hash_map with HASH_MAP_TYPE_PTR associates pointer values (<code>void *</code>) with the
 * data referenced by pointer keys (<code>void *</code>). <code>NULL</code> pointer values are not
 * supported. A <code>NULL</code> key value is supported when the instance's key comparator and
 * hasher functions support it.
 *
 * The map is implemented as hash table, which should provide constant-time insert, query, and
 * remove operations, although the insert may occasionally grow the table, which is linear in the
 * number of entries in the map. The table will grow as needed to hold new entries, but will not
 * shrink as entries are removed.
 *
 * The key and value pointers passed to the map are retained and used by the map, but are not owned
 * by the map. Freeing the map does not attempt to free the pointers. The client is entirely
 * responsible for the memory management of the keys and values. The current interface and
 * implementation assume that keys will be properties of the values, or that keys will not be
 * memory managed, or that keys will not need to be freed as a result of being replaced when a key
 * is re-mapped.
 */

struct vdo_hash_map;

typedef union HashKey {
        const void *ptr_key;
        u64 int_key;
} vdo_hash_key;

enum vdo_hash_map_type {
	HASH_MAP_TYPE_INT		 = 0,
	HASH_MAP_TYPE_PTR	         = 1,
};

int __must_check vdo_hash_map_create(enum vdo_hash_map_type,
				     size_t initial_capacity,
				     struct vdo_hash_map **map_ptr);

void vdo_hash_map_free(struct vdo_hash_map *map);

size_t vdo_hash_map_size(const struct vdo_hash_map *map);

void *vdo_hash_map_get(struct vdo_hash_map *map, const vdo_hash_key *key);

int __must_check vdo_hash_map_put(struct vdo_hash_map *map,
				  const vdo_hash_key *key, void *new_value,
				  bool update, void **old_value_ptr);

void *vdo_hash_map_remove(struct vdo_hash_map *map, const vdo_hash_key *key);

#endif /* VDO_HAS_MAP_H */
