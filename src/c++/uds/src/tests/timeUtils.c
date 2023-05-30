// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "testPrototypes.h"

#include "string-utils.h"
#include "time-utils.h"

int rel_time_to_string(char **strp, ktime_t reltime)
{
	const char *sign;
	const char *units;
	unsigned long value;

	if (reltime < 0) {
		/* Ensure that the code behaves well with negative time. */
		sign = "-";
		reltime = -reltime;
	} else {
		sign = "";
	}
	if (reltime > seconds_to_ktime(1)) {
		units = "seconds";
		value = ktime_to_ms(reltime);
	} else if (reltime > ms_to_ktime(1)) {
		units = "milliseconds";
		value = ktime_to_us(reltime);
	} else {
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

#ifdef __KERNEL__
void sleep_for(ktime_t reltime)
{
	unsigned long rt = 1 + ktime_to_us(reltime);

	usleep_range(rt, rt);
}
#else
void sleep_for(ktime_t reltime)
{
	int ret;
	struct timespec duration;
	struct timespec remaining;

	if (reltime < 0) {
		return;
	}
	remaining.tv_sec = reltime / NSEC_PER_SEC;
	remaining.tv_nsec = reltime % NSEC_PER_SEC;
	do {
		duration = remaining;
		ret = nanosleep(&duration, &remaining);
	} while ((ret == -1) && (errno == EINTR));
}
#endif /* __KERNEL__ */
