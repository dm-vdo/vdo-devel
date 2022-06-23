/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_PERF_COMMON_H
#define INDEX_PERF_COMMON_H

#include "oldInterfaces.h"

typedef struct {
  uint64_t  nameCounter;
  void     *private;
} FillState;

/**
 * A function that produces a deterministic sequence of numbers to
 * be hashed into chunkNames.
 **/
typedef uint64_t (*fillFunc)(FillState *);

/**
 * The fill-function that just monotonically increases the name counter.
 **/
uint64_t newData(FillState *state);

/**
 * write blocks into the index with the pattern specified by the
 * BlockGeneratorPattern
 **/
void fill(const char               *label,
          struct uds_index_session *indexSession,
          unsigned int              outerCount,
          unsigned int              innerCount,
          fillFunc                  nextBlock,
          FillState                *state,
          OldDedupeBlockCallback    callback);

#endif /* INDEX_PERF_COMMON_H */
