// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "permassert.h"
#include "string-utils.h"
#include "time-utils.h"

#ifdef __KERNEL__
#include <linux/delay.h>
#include <linux/ktime.h>
#else
#include <errno.h>
#endif

#ifndef __KERNEL__
ktime_t current_time_ns(clockid_t clock)
{
	struct timespec ts;

	if (clock_gettime(clock, &ts) != 0) {
		ts = (struct timespec){ 0, 0 };
	}
	return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

struct timespec future_time(ktime_t offset)
{
	ktime_t future = current_time_ns(CLOCK_REALTIME) + offset;

	return (struct timespec){
		.tv_sec = future / NSEC_PER_SEC,
		.tv_nsec = future % NSEC_PER_SEC,
	};
}
#endif /* __KERNEL__ */

int64_t current_time_us(void)
{
	return current_time_ns(CLOCK_REALTIME) / NSEC_PER_USEC;
}

#ifdef TEST_INTERNAL
int rel_time_to_string(char **strp, ktime_t reltime, long counter)
{
	const char *sign, *units;
	unsigned long value;
	/*
	 * If there is a counter, divide the time by the counter.  This is
	 * intended for reporting values of time per operation.
	 */
	if (counter > 0) {
		reltime /= counter;
	}

	if (reltime < 0) {
		/*
		 * Negative time is unusual, but ensure that the rest of the
		 * code behaves well.
		 */
		sign = "-";
		reltime = -reltime;
	} else {
		sign = "";
	}
	if (reltime > seconds_to_ktime(1)) {
		/* Larger than a second, so report to millisecond accuracy */
		units = "seconds";
		value = ktime_to_ms(reltime);
	} else if (reltime > ms_to_ktime(1)) {
		/* Larger than a millisecond, so report to microsecond accuracy */
		units = "milliseconds";
		value = ktime_to_us(reltime);
	} else {
		/* Report to nanosecond accuracy */
		units = "microseconds";
		value = reltime;
	}

	return uds_alloc_sprintf(__func__,
				 strp,
				 "%s%ld.%03ld %s",
				 sign,
				 value / 1000,
				 value % 1000,
				 units);
}
#endif /* TEST_INTERNAL */

#ifdef TEST_INTERNAL
void sleep_for(ktime_t reltime)
{
#ifdef __KERNEL__
	unsigned long rt = 1 + ktime_to_us(reltime);

	usleep_range(rt, rt);
#else
	int ret;
	struct timespec duration, remaining;

	if (reltime < 0) {
		return;
	}
	remaining.tv_sec = reltime / NSEC_PER_SEC;
	remaining.tv_nsec = reltime % NSEC_PER_SEC;
	do {
		duration = remaining;
		ret = nanosleep(&duration, &remaining);
	} while ((ret == -1) && (errno == EINTR));
#endif
}
#endif /* TEST_INTERNAL */

#ifdef TEST_INTERNAL
#ifndef __KERNEL__
void timeStaticAssertion(void)
{
	STATIC_ASSERT(sizeof(time_t) == sizeof(int64_t));
}
#endif /* __KERNEL__ */
#endif /* TEST_INTERNAL */
