/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef RESOURCE_USAGE_H
#define RESOURCE_USAGE_H 1

#include "compiler.h"
#include "time-utils.h"

#ifndef __KERNEL__
#include <sys/resource.h>
#endif

#ifdef __KERNEL__
typedef int ResourceUsage;
typedef struct threadStatistics ThreadStatistics;
#else
typedef struct rusage ResourceUsage;
typedef struct threadStatistics ThreadStatistics;
#endif

/**
 * Get a snapshot of the system resource usage
 *
 * @param ru  The snapshot
 *
 * @return UDS_SUCCESS or a system error code
 **/
static inline int getResourceUsage(ResourceUsage *ru)
{
#ifdef __KERNEL__
  return UDS_SUCCESS;
#else
  return getrusage(RUSAGE_SELF, ru);
#endif
}

/**
 * Free a snapshot of the system thread statistics
 *
 * @param ts  Thread statistics
 */
void freeThreadStatistics(ThreadStatistics *ts);

/**
 * Get a snapshot of the system thread statistics
 *
 * @return the system thread statistics
 **/
ThreadStatistics *getThreadStatistics(void);

/**
 * Print stats on resource usage over some interval.
 *
 * Usage:
 *   ktime_t then = current_time_ns(CLOCK_REALTIME);
 *   ResourceUsage thenUsage;
 *   getResourceUsage(&thenUsage);
 *
 *   // do some stuff
 *   ktime_t elapsed = ktime_sub(current_time_ns(CLOCK_REALTIME), then);
 *   ResourceUsage nowUsage;
 *   getResourceUsage(&nowUsage);
 *
 *   // print usage over the period.
 *   printResourceUsage(&thenUsage, &nowUsage, elapsed);
 *
 * @param prev    Previously recorded resource usage
 * @param cur     Resource usage at end of interval
 * @param elapsed Length of interval
 */
#ifdef __KERNEL__
static inline void printResourceUsage(ResourceUsage *prev,
                                      ResourceUsage *cur,
                                      ktime_t        elapsed)
{
}
#else
void printResourceUsage(ResourceUsage *prev,
                        ResourceUsage *cur,
                        ktime_t        elapsed);
#endif

/**
 * Print stats on thread usage over some interval.
 *
 * Usage:
 *   ThreadStatistics *preThreadStats = getThreadStatistics();
 *
 *   // do some stuff
 *   ThreadStatistics *postThreadStats = getThreadStatistics();
 *
 *   // print usage over the period.
 *   printThreadStatistics(preThreadStats, postThreadStats);
 *   freeThreadStats(postThreadStats);
 *   freeThreadStats(preThreadStats);
 *
 * @param prev Thread statistics at the start of the interval
 * @param cur  Thread statistics at the end of the interval
 */
void printThreadStatistics(ThreadStatistics *prev, ThreadStatistics *cur);

/**
 * Report VM stuff of interest, to stdout.
 **/
#ifdef __KERNEL__
static inline void printVmStuff(void)
{
}
#else
void printVmStuff(void);
#endif

#endif /* RESOURCE_USAGE_H */
