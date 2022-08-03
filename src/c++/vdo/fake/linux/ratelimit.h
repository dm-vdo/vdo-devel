/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_RATELIMIT_H
#define LINUX_RATELIMIT_H

#define RATELIMIT_MSG_ON_RELEASE	1

struct ratelimit_state {
	int interval;
};

// For unit tests, no rate limiting
static inline bool
__ratelimit(struct ratelimit_state *rs __attribute__((unused)))
{
	return false;
}

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
