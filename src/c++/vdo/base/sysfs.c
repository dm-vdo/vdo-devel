// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/module.h>

#include "logger.h"

#include "constants.h"
#include "dedupe.h"
#include "vdo.h"

static int vdo_log_level_show(char *buf,
			      const struct kernel_param *kp)
{
	return sprintf(buf, "%s\n", uds_log_priority_to_string(get_uds_log_level()));
}

static int vdo_log_level_store(const char *buf,
			       const struct kernel_param *kp)
{
	static char internal_buf[11];

	int n = strlen(buf);

	if (n > 10)
		return -EINVAL;

	memset(internal_buf, '\000', sizeof(internal_buf));
	memcpy(internal_buf, buf, n);
	if (internal_buf[n - 1] == '\n')
		internal_buf[n - 1] = '\000';
	set_uds_log_level(uds_log_string_to_priority(internal_buf));
	return 0;
}

#ifdef VDO_INTERNAL
static int vdo_max_req_active_store(const char *buf,
				    const struct kernel_param *kp)
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

static const struct kernel_param_ops log_level_ops = {
	.set = vdo_log_level_store,
	.get = vdo_log_level_show,
};

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

module_param_cb(log_level, &log_level_ops, NULL, 0644);

#ifdef VDO_INTERNAL
module_param_cb(max_requests_active, &requests_ops,
		&data_vio_count, 0644);
#endif //VDO_INTERNAL

module_param_cb(deduplication_timeout_interval, &dedupe_timeout_ops,
		&vdo_dedupe_index_timeout_interval, 0644);

module_param_cb(min_deduplication_timer_interval, &dedupe_timer_ops,
		&vdo_dedupe_index_min_timer_interval, 0644);
