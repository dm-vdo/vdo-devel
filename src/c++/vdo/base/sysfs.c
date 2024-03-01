// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/module.h>

#include "logger.h"

#include "constants.h"
#include "dedupe.h"
#include "vdo.h"

#ifdef VDO_INTERNAL
static int vdo_max_req_active_store(const char *buf, const struct kernel_param *kp)
{
	int result = param_set_int(buf, kp);
	unsigned int value;

	if (result != 0)
		return result;

	value = *(int *)kp->arg;

	if (value < 1)
		value = 1;
	else if (value > MAXIMUM_VDO_USER_VIOS)
		value = MAXIMUM_VDO_USER_VIOS;
	*(int *)kp->arg = value;
	return 0;
}
#endif //VDO_INTERNAL

static int vdo_dedupe_timeout_interval_store(const char *buf,
					     const struct kernel_param *kp)
{
	int result = param_set_uint(buf, kp);

	if (result != 0)
		return result;
	vdo_set_dedupe_index_timeout_interval(*(uint *)kp->arg);
	return 0;
}

static int vdo_min_dedupe_timer_interval_store(const char *buf,
					       const struct kernel_param *kp)
{
	int result = param_set_uint(buf, kp);

	if (result != 0)
		return result;
	vdo_set_dedupe_index_min_timer_interval(*(uint *)kp->arg);
	return 0;
}

#ifdef VDO_INTERNAL
static const struct kernel_param_ops requests_ops = {
	.set = vdo_max_req_active_store,
	.get = param_get_int,
};
#endif //VDO_INTERNAL

static const struct kernel_param_ops dedupe_timeout_ops = {
	.set = vdo_dedupe_timeout_interval_store,
	.get = param_get_uint,
};

static const struct kernel_param_ops dedupe_timer_ops = {
	.set = vdo_min_dedupe_timer_interval_store,
	.get = param_get_uint,
};

#ifdef VDO_INTERNAL
module_param_cb(max_requests_active, &requests_ops, &data_vio_count, 0644);
#endif //VDO_INTERNAL

module_param_cb(deduplication_timeout_interval, &dedupe_timeout_ops,
		&vdo_dedupe_index_timeout_interval, 0644);

module_param_cb(min_deduplication_timer_interval, &dedupe_timer_ops,
		&vdo_dedupe_index_min_timer_interval, 0644);
