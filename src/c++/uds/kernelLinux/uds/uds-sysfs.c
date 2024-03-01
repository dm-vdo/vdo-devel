// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "uds-sysfs.h"

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"

#include "indexer.h"

#define UDS_SYSFS_NAME "uds"

static struct {
	/* /sys/uds */
	struct kobject kobj;
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	/* /sys/uds/memory */
	struct kobject memory_kobj;
#endif /* TEST_INTERNAL or VDO_INTERNAL */

	/* These flags are used to ensure a clean shutdown */

	/* /sys/uds flag */
	bool flag;
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	/* /sys/uds/memory flag */
	bool memory_flag;
#endif /* TEST_INTERNAL or VDO_INTERNAL */
} object_root;

/*
 * This is the code for any directory in the /sys/<module_name> tree that contains no regular files
 * (only subdirectories).
 */

static void empty_release(struct kobject *kobj)
{
}

static ssize_t empty_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	return 0;
}

static ssize_t empty_store(struct kobject *kobj, struct attribute *attr, const char *buf,
			   size_t length)
{
	return length;
}

static const struct sysfs_ops empty_ops = {
	.show = empty_show,
	.store = empty_store,
};

static struct attribute *empty_attrs[] = {
	NULL,
};
ATTRIBUTE_GROUPS(empty);

static const struct kobj_type empty_object_type = {
	.release = empty_release,
	.sysfs_ops = &empty_ops,
	.default_groups = empty_groups,
};

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
/*
 * This is the code for the /sys/<module_name>/memory directory.
 *
 * <dir>/allocation_counter
 * <dir>/error_injection_counter
 *
 * <dir>/cancel_allocation_failure
 * <dir>/log_allocations
 *
 * <dir>/schedule_allocation_failure
 * <dir>/track_allocations
 */

struct memory_attribute {
	struct attribute attr;
	long (*show_long)(void);
	void (*store)(void);
	void (*store_long)(long);
};

static ssize_t memory_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct memory_attribute *ma;

	ma = container_of(attr, struct memory_attribute, attr);
	if (ma->show_long != NULL)
		return sprintf(buf, "%ld\n", ma->show_long());
	else
		return -EINVAL;
}

static ssize_t memory_store(struct kobject *kobj, struct attribute *attr,
			    const char *buf, size_t length)
{
	struct memory_attribute *ma;

	ma = container_of(attr, struct memory_attribute, attr);
	if (ma->store != NULL) {
		ma->store();
	} else if (ma->store_long != NULL) {
		long value;

		if (sscanf(buf, "%ld", &value) != 1)
			return -EINVAL;
		ma->store_long(value);
	} else {
		return -EINVAL;
	}

	return length;
}

static long memory_show_allocation_counter(void)
{
	return atomic_long_read(&uds_allocate_memory_counter);
}

static long memory_show_bytes_used(void)
{
	u64 bytes_used;
	u64 peak_bytes_used;

	vdo_get_memory_stats(&bytes_used, &peak_bytes_used);
	return bytes_used;
}

static long memory_show_error_injection_counter(void)
{
	return uds_allocation_error_injection;
}

static void memory_store_cancel_allocation_failure(void)
{
	cancel_uds_memory_allocation_failure();
}

static void memory_store_log_allocations(void)
{
	log_uds_memory_allocations();
}

static void memory_store_schedule_allocation_failure(long value)
{
	schedule_uds_memory_allocation_failure(value);
}

static void memory_store_track_allocations(long value)
{
	track_uds_memory_allocations(value != 0);
}

static struct memory_attribute allocation_counter_attr = {
	.attr = { .name = "allocation_counter", .mode = 0444 },
	.show_long = memory_show_allocation_counter,
};

static struct memory_attribute bytes_used_attr = {
	.attr = { .name = "bytes_used", .mode = 0444 },
	.show_long = memory_show_bytes_used,
};

static struct memory_attribute cancel_allocation_failure_attr = {
	.attr = { .name = "cancel_allocation_failure", .mode = 0200 },
	.store = memory_store_cancel_allocation_failure,
};

static struct memory_attribute error_injection_counter_attr = {
	.attr = { .name = "error_injection_counter", .mode = 0444 },
	.show_long = memory_show_error_injection_counter,
};

static struct memory_attribute log_allocations_attr = {
	.attr = { .name = "log_allocations", .mode = 0200 },
	.store = memory_store_log_allocations,
};

static struct memory_attribute schedule_allocation_failure_attr = {
	.attr = { .name = "schedule_allocation_failure", .mode = 0200 },
	.store_long = memory_store_schedule_allocation_failure,
};

static struct memory_attribute track_allocations_attr = {
	.attr = { .name = "track_allocations", .mode = 0200 },
	.store_long = memory_store_track_allocations,
};

static struct attribute *memory_attrs[] = {
	&allocation_counter_attr.attr,
	&bytes_used_attr.attr,
	&cancel_allocation_failure_attr.attr,
	&log_allocations_attr.attr,
	&error_injection_counter_attr.attr,
	&schedule_allocation_failure_attr.attr,
	&track_allocations_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(memory);

static const struct sysfs_ops memory_ops = {
	.show = memory_show,
	.store = memory_store,
};

static const struct kobj_type memory_object_type = {
	.release = empty_release,
	.sysfs_ops = &memory_ops,
	.default_groups = memory_groups,
};

#endif /* TEST_INTERNAL or VDO_INTERNAL */
int uds_init_sysfs(void)
{
	int result;

	memset(&object_root, 0, sizeof(object_root));
	kobject_init(&object_root.kobj, &empty_object_type);
	result = kobject_add(&object_root.kobj, NULL, UDS_SYSFS_NAME);
	if (result == 0)
		object_root.flag = true;

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	if (result == 0) {
		kobject_init(&object_root.memory_kobj, &memory_object_type);
		result = kobject_add(&object_root.memory_kobj, &object_root.kobj,
				     "memory");
		if (result == 0)
			object_root.memory_flag = true;
	}

#endif /* TEST_INTERNAL or VDO_INTERNAL */
	if (result != 0)
		uds_put_sysfs();

	return result;
}

void uds_put_sysfs(void)
{
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	if (object_root.memory_flag)
		kobject_put(&object_root.memory_kobj);

#endif /* TEST_INTERNAL or VDO_INTERNAL */
	if (object_root.flag)
		kobject_put(&object_root.kobj);
}
