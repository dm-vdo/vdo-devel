/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test implementations of linux/module.h.
 *
 * Presumes there is only one module (vdo).
 *
 * Copyright Red Hat
 *
 */

#ifndef __LINUX_MODULE_H
#define __LINUX_MODULE_H

#define __init
#define __exit

typedef int module_initializer(void);
typedef void module_exiter(void);

extern module_initializer *vdo_module_initialize;
extern module_exiter *vdo_module_exit;

#define module_init(fn) module_initializer *vdo_module_initialize = fn
#define module_exit(fn) module_exiter *vdo_module_exit = fn

#endif // __LINUX_MODULE_H
