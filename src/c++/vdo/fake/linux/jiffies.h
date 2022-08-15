/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_JIFFIES_H
#define LINUX_JIFFIES_H

#include "types.h"

enum {
	MS_PER_JIFFY = 4,
	US_PER_JIFFY = MS_PER_JIFFY * 1000,
};

unsigned long getUnitTestJiffies(void);

#define jiffies (getUnitTestJiffies() / 1)

static inline unsigned long msecs_to_jiffies(const unsigned int m)
{
	return m / MS_PER_JIFFY;
}

static inline unsigned int jiffies_to_msecs(const unsigned long j)
{
	return j * MS_PER_JIFFY;
}

#endif /* LINUX_JIFFIES_H */
