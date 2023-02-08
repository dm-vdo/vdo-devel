/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#ifdef __KERNEL__
#include <linux/ktime.h>
#include <linux/time.h>
#else
#include <linux/compiler.h>
#endif /* __KERNEL__ */
#include <linux/types.h>
#ifndef __KERNEL__
#include <sys/time.h>
#include <time.h>

/* Some constants that are defined in kernel headers. */
#define NSEC_PER_SEC 1000000000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_USEC 1000L

typedef s64 ktime_t;
#endif /* !__KERNEL__ */

static inline s64 ktime_to_seconds(ktime_t reltime)
{
	return reltime / NSEC_PER_SEC;
}

#ifdef __KERNEL__
static inline ktime_t current_time_ns(clockid_t clock)
{
	return clock == CLOCK_MONOTONIC ? ktime_get_ns() : ktime_get_real_ns();
}

static inline ktime_t current_time_us(void)
{
	return current_time_ns(CLOCK_REALTIME) / NSEC_PER_USEC;
}
#else
ktime_t __must_check current_time_ns(clockid_t clock);

ktime_t __must_check current_time_us(void);

/* Return a timespec for the current time plus an offset. */
struct timespec future_time(ktime_t offset);

static inline ktime_t ktime_sub(ktime_t a, ktime_t b)
{
	return a - b;
}

static inline s64 ktime_to_ms(ktime_t abstime)
{
	return abstime / NSEC_PER_MSEC;
}

static inline ktime_t ms_to_ktime(u64 milliseconds)
{
	return (ktime_t) milliseconds * NSEC_PER_MSEC;
}

static inline s64 ktime_to_us(ktime_t reltime)
{
	return reltime / NSEC_PER_USEC;
}
#endif /* __KERNEL__ */

#endif /* TIME_UTILS_H */
