/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/mempool.h.
 *
 * Copyright (C) ??? (not sure what to put here)
 * Copyright 2023 Red Hat
 *
 */
#ifndef _LINUX_MEMPOOL_H
#define _LINUX_MEMPOOL_H

#include "types.h"

typedef void * (mempool_alloc_t)(gfp_t gfp_mask, void *pool_data);
typedef void (mempool_free_t)(void *element, void *pool_data);

typedef struct mempool mempool_t;

mempool_t *mempool_create(int min_nr,
			  mempool_alloc_t *alloc_fn,
			  mempool_free_t *free_fn,
			  void *pool_data);
void mempool_destroy(mempool_t *pool);
void *mempool_alloc(mempool_t *pool, gfp_t gfp_mask);
void mempool_free(void *element, mempool_t *pool);

#endif /* _LINUX_MEMPOOL_H */
