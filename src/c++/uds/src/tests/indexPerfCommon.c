// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "indexPerfCommon.h"

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "memory-alloc.h"
#include "oldInterfaces.h"
#include "resourceUsage.h"
#include "testPrototypes.h"
#include "time-utils.h"

uint64_t newData(FillState *state)
{
  return state->nameCounter++;
}

void fill(const char               *label,
          struct uds_index_session *indexSession,
          unsigned int              outerCount,
          unsigned int              innerCount,
          fillFunc                  nextBlock,
          FillState                *state,
          OldDedupeBlockCallback    callback)
{
  unsigned long totalBlocks = 0;
  ktime_t totalElapsed = 0;
  ThreadStatistics *preThreadStats = getThreadStatistics();

  unsigned int innerIndex, outerIndex;
  for (outerIndex = 0; outerIndex < outerCount; outerIndex++) {
    ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
    ResourceUsage curResUsage, prevResUsage;
    getResourceUsage(&prevResUsage);

    for (innerIndex = 0; innerIndex < innerCount; innerIndex++) {
      uint64_t counter = nextBlock(state);
      struct uds_record_name chunkName
        = hash_record_name(&counter, sizeof(counter));
      oldPostBlockName(indexSession, NULL,
                       (struct uds_record_data *) &chunkName,
                       &chunkName, callback);
    }

    UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
    ktime_t loopStop = current_time_ns(CLOCK_MONOTONIC);
    ktime_t loopElapsed = ktime_sub(loopStop, loopStart);

    totalBlocks  += innerCount;
    totalElapsed += loopElapsed;

    getResourceUsage(&curResUsage);
    {
      struct uds_index_stats stats;
      UDS_ASSERT_SUCCESS(uds_get_index_session_stats(indexSession, &stats));

      printResourceUsage(&prevResUsage, &curResUsage, loopElapsed);
      prevResUsage = curResUsage;
      char *loopAll, *loopEach, *totalAll, *totalEach;
      UDS_ASSERT_SUCCESS(rel_time_to_string(&loopAll, loopElapsed));
      UDS_ASSERT_SUCCESS(rel_time_to_string(&loopEach,
                                            loopElapsed /innerCount));
      UDS_ASSERT_SUCCESS(rel_time_to_string(&totalAll, totalElapsed));
      UDS_ASSERT_SUCCESS(rel_time_to_string(&totalEach,
                                            totalElapsed / totalBlocks));
      albPrint("%s Last:  %10d blocks took %s at %s/block",
               label, innerCount, loopAll, loopEach);
      albPrint("%s Total: %10ld blocks took %s at %s/block",
               label, totalBlocks, totalAll, totalEach);
      albPrint("Index entries: %llu, discards: %llu, collisions: %llu",
               (unsigned long long) stats.entries_indexed,
               (unsigned long long) stats.entries_discarded,
               (unsigned long long) stats.collisions);
      uds_free(loopAll);
      uds_free(loopEach);
      uds_free(totalAll);
      uds_free(totalEach);
    }
    albFlush();
  }
  ThreadStatistics *postThreadStats = getThreadStatistics();
  printVmStuff();
  printThreadStatistics(preThreadStats, postThreadStats);
  albFlush();

  freeThreadStatistics(postThreadStats);
  freeThreadStatistics(preThreadStats);
}
