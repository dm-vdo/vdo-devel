/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2024 Red Hat
 *
 */

#ifndef LINUX_RATELIMIT_H
#define LINUX_RATELIMIT_H

#include <linux/linuxTypes.h>

#define RATELIMIT_MSG_ON_RELEASE	BIT(0)

struct ratelimit_state {
	int             interval;
	unsigned long	flags;
};

// For unit tests, no rate limiting
static inline int
___ratelimit(struct ratelimit_state *rs __attribute__((unused)),
	     const char *func __attribute__((unused)))
{
	return 0;
}

#define __ratelimit(state) ___ratelimit(state, __func__)

static inline void
ratelimit_default_init(struct ratelimit_state *rs __attribute__((unused)))
{
}

static inline void
ratelimit_set_flags(struct ratelimit_state *rs __attribute__((unused)),
		    unsigned long flags __attribute__((unused)))
{
}

static inline void
ratelimit_state_exit(struct ratelimit_state *rs __attribute__((unused)))
{
}

#endif /* LINUX_RATELIMIT_H */
