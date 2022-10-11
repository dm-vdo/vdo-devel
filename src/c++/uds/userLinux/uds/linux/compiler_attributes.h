/* SPDX-License-Identifier: GPL-2.0 */
/*
 * User space attribute defines to match the ones in the kernel's
 * linux/compiler_attributes.h
 *
 * Copyright Red Hat
 */
#ifndef __LINUX_COMPILER_ATTRIBUTES_H
#define __LINUX_COMPILER_ATTRIBUTES_H

#define __always_unused __attribute__((unused))
#define __maybe_unused  __attribute__((unused))
#define __must_check    __attribute__((warn_unused_result))
#define noinline        __attribute__((__noinline__))
#define __packed        __attribute__((packed))
#define __printf(a, b)  __attribute__((__format__(printf, a, b)))
#define __aligned(x)    __attribute__((__aligned__(x)))

#endif /* __LINUX_COMPILER_ATTRIBUTES_H */
