// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "uds-sysfs.h"

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "uds.h"

#define UDS_SYSFS_NAME "uds"

static struct {
	/* /sys/uds */
	struct kobject kobj;
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	/* /sys/uds/memory */
	struct kobject memory_kobj;
#endif /* TEST_INTERNAL or VDO_INTERNAL */
	/* /sys/uds/parameter */
	struct kobject parameter_kobj;

	/* These flags are used to ensure a clean shutdown */

	/* /sys/uds flag */
	bool flag;
#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	/* /sys/uds/memory flag */
	bool memory_flag;
#endif /* TEST_INTERNAL or VDO_INTERNAL */
	/* /sys/uds/parameter flag */
	bool parameter_flag;
} object_root;

static char *buffer_to_string(const char *buf, size_t length)
{
	char *string;

	if (UDS_ALLOCATE(length + 1, char, __func__, &string) != UDS_SUCCESS)
		return NULL;

	memcpy(string, buf, length);
	string[length] = '\0';
	if (string[length - 1] == '\n')
		string[length - 1] = '\0';

	return string;
}

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

static ssize_t
empty_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t length)
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

static struct kobj_type empty_object_type = {
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

static ssize_t
memory_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t length)
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
	uint64_t bytes_used;
	uint64_t peak_bytes_used;

	get_uds_memory_stats(&bytes_used, &peak_bytes_used);
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

static struct kobj_type memory_object_type = {
	.release = empty_release,
	.sysfs_ops = &memory_ops,
	.default_groups = memory_groups,
};
#endif /* TEST_INTERNAL or VDO_INTERNAL */

/*
 * This is the code for the /sys/<module_name>/parameter directory.
 * <dir>/log_level                 UDS_LOG_LEVEL
 */

struct parameter_attribute {
	struct attribute attr;
	const char *(*show_string)(void);
	void (*store_string)(const char *string);
};

static ssize_t parameter_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct parameter_attribute *pa;

	pa = container_of(attr, struct parameter_attribute, attr);
	if (pa->show_string != NULL)
		return sprintf(buf, "%s\n", pa->show_string());
	else
		return -EINVAL;
}

static ssize_t
parameter_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t length)
{
	char *string;
	struct parameter_attribute *pa;

	pa = container_of(attr, struct parameter_attribute, attr);
	if (pa->store_string == NULL)
		return -EINVAL;
	string = buffer_to_string(buf, length);
	if (string == NULL)
		return -ENOMEM;

	pa->store_string(string);
	UDS_FREE(string);
	return length;
}

static const char *parameter_show_log_level(void)
{
	return uds_log_priority_to_string(get_uds_log_level());
}

static void parameter_store_log_level(const char *string)
{
	set_uds_log_level(uds_log_string_to_priority(string));
}

static struct parameter_attribute log_level_attr = {
	.attr = { .name = "log_level", .mode = 0600 },
	.show_string = parameter_show_log_level,
	.store_string = parameter_store_log_level,
};

static struct attribute *parameter_attrs[] = {
	&log_level_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(parameter);

static const struct sysfs_ops parameter_ops = {
	.show = parameter_show,
	.store = parameter_store,
};

static struct kobj_type parameter_object_type = {
	.release = empty_release,
	.sysfs_ops = &parameter_ops,
	.default_groups = parameter_groups,
};

int uds_init_sysfs(void)
{
	int result;

	memset(&object_root, 0, sizeof(object_root));
	kobject_init(&object_root.kobj, &empty_object_type);
	result = kobject_add(&object_root.kobj, NULL, UDS_SYSFS_NAME);
	if (result == 0) {
		object_root.flag = true;
		kobject_init(&object_root.parameter_kobj, &parameter_object_type);
		result = kobject_add(&object_root.parameter_kobj, &object_root.kobj, "parameter");
		if (result == 0)
			object_root.parameter_flag = true;
	}

#if defined(TEST_INTERNAL) || defined(VDO_INTERNAL)
	if (result == 0) {
		kobject_init(&object_root.memory_kobj, &memory_object_type);
		result = kobject_add(&object_root.memory_kobj, &object_root.kobj, "memory");
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
	if (object_root.parameter_flag)
		kobject_put(&object_root.parameter_kobj);

	if (object_root.flag)
		kobject_put(&object_root.kobj);
}
