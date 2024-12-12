/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_POOL_SYSFS_H
#define VDO_POOL_SYSFS_H

#include <linux/kobject.h>

/* The kobj_type used for setting up the kernel layer kobject. */
extern const struct kobj_type vdo_directory_type;

#endif /* VDO_POOL_SYSFS_H */
