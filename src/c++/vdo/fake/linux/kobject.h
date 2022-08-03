/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef KOBJECT_H
#define KOBJECT_H

#include <linux/atomic.h>

#include "list.h"
#include "types.h"

struct kobject;

struct attribute {
	char *name;
	int mode;
};

struct attribute_group {
	struct attribute **attrs;
};

#define __ATTRIBUTE_GROUPS(_name)				\
static const struct attribute_group *_name##_groups[] = {	\
	&_name##_group,						\
	NULL,							\
}

#define ATTRIBUTE_GROUPS(_name)					\
static const struct attribute_group _name##_group = {		\
	.attrs = _name##_attrs,					\
};								\
__ATTRIBUTE_GROUPS(_name)

typedef ssize_t sysfs_op_store(struct kobject *directory,
			       struct attribute *attr,
			       const char *buf,
			       size_t length);
typedef ssize_t sysfs_op_show(struct kobject *directory,
			      struct attribute *attr,
			      char *buf);

struct sysfs_ops {
	sysfs_op_store *store;
	sysfs_op_show *show;
};

struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
};

struct kobject {
	char		        *name;
	struct kobject		*parent;
	struct kobj_type	*ktype;
	unsigned int		 state_initialized:1;
	atomic_t		 refcount;
};

struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

__must_check
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...)
__attribute__((format(printf, 3, 4)));

struct kobject *kobject_get(struct kobject *kobj);

void kobject_put(struct kobject *kobj);

void kobject_init(struct kobject *kobj, struct kobj_type *ktype);

/* The global /sys/kernel/ kobject for people to chain off of */
extern struct kobject *kernel_kobj;

/**
 * Initialize kernel_kobj. This method exists for unit tests when run with
 * --no-fork since some tests can't easily clean up after themselves.
 **/
void initialize_kernel_kobject(void);

#endif /* KOBJECT_H */
