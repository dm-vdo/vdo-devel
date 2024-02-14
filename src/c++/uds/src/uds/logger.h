/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_LOGGER_H
#define VDO_LOGGER_H

#ifdef __KERNEL__
#include <linux/kern_levels.h>
#include <linux/module.h>
#include <linux/ratelimit.h>
#include <linux/device-mapper.h>
#else
#include <stdarg.h>
#include "minisyslog.h"
#endif

/* Custom logging utilities for UDS */

#ifdef __KERNEL__
enum {
	VDO_LOG_EMERG = LOGLEVEL_EMERG,
	VDO_LOG_ALERT = LOGLEVEL_ALERT,
	VDO_LOG_CRIT = LOGLEVEL_CRIT,
	VDO_LOG_ERR = LOGLEVEL_ERR,
	VDO_LOG_WARNING = LOGLEVEL_WARNING,
	VDO_LOG_NOTICE = LOGLEVEL_NOTICE,
	VDO_LOG_INFO = LOGLEVEL_INFO,
	VDO_LOG_DEBUG = LOGLEVEL_DEBUG,

	VDO_LOG_MAX = VDO_LOG_DEBUG,
	VDO_LOG_DEFAULT = VDO_LOG_INFO,
};

extern int vdo_log_level;
#else
#define UDS_LOG_EMERG LOG_EMERG
#define UDS_LOG_ALERT LOG_ALERT
#define UDS_LOG_CRIT LOG_CRIT
#define UDS_LOG_ERR LOG_ERR
#define UDS_LOG_WARNING LOG_WARNING
#define UDS_LOG_NOTICE LOG_NOTICE
#define UDS_LOG_INFO LOG_INFO
#define UDS_LOG_DEBUG LOG_DEBUG
#endif /* __KERNEL__ */

#ifdef __KERNEL__
#define DM_MSG_PREFIX "vdo"
#define VDO_LOGGING_MODULE_NAME DM_NAME ": " DM_MSG_PREFIX
#else /* userspace */
#define UDS_LOGGING_MODULE_NAME "vdo"
#endif /* __KERNEL__ */

/* Apply a rate limiter to a log method call. */
#ifdef __KERNEL__
#define vdo_log_ratelimit(log_fn, ...)                                    \
	do {                                                              \
		static DEFINE_RATELIMIT_STATE(_rs,                        \
					      DEFAULT_RATELIMIT_INTERVAL, \
					      DEFAULT_RATELIMIT_BURST);   \
		if (__ratelimit(&_rs)) {                                  \
			log_fn(__VA_ARGS__);                              \
		}                                                         \
	} while (0)
#else
#define uds_log_ratelimit(log_fn, ...) log_fn(__VA_ARGS__)
#endif /* __KERNEL__ */

int vdo_get_log_level(void);

#ifndef __KERNEL__
int uds_log_string_to_priority(const char *string);

const char *uds_log_priority_to_string(int priority);

#endif
void vdo_log_embedded_message(int priority, const char *module, const char *prefix,
			      const char *fmt1, va_list args1, const char *fmt2, ...)
	__printf(4, 0) __printf(6, 7);

void vdo_log_backtrace(int priority);

/* All log functions will preserve the caller's value of errno. */

#define vdo_log_strerror(priority, errnum, ...) \
	__vdo_log_strerror(priority, errnum, VDO_LOGGING_MODULE_NAME, __VA_ARGS__)

int __vdo_log_strerror(int priority, int errnum, const char *module,
		       const char *format, ...)
	__printf(4, 5);

int vdo_vlog_strerror(int priority, int errnum, const char *module, const char *format,
		      va_list args)
	__printf(4, 0);

/* Log an error prefixed with the string associated with the errnum. */
#define vdo_log_error_strerror(errnum, ...) \
	vdo_log_strerror(VDO_LOG_ERR, errnum, __VA_ARGS__)

#define vdo_log_debug_strerror(errnum, ...) \
	vdo_log_strerror(VDO_LOG_DEBUG, errnum, __VA_ARGS__)

#define vdo_log_info_strerror(errnum, ...) \
	vdo_log_strerror(VDO_LOG_INFO, errnum, __VA_ARGS__)

#define vdo_log_warning_strerror(errnum, ...) \
	vdo_log_strerror(VDO_LOG_WARNING, errnum, __VA_ARGS__)

#define vdo_log_fatal_strerror(errnum, ...) \
	vdo_log_strerror(VDO_LOG_CRIT, errnum, __VA_ARGS__)

#ifdef __KERNEL__
#define vdo_log_message(priority, ...) \
	__vdo_log_message(priority, VDO_LOGGING_MODULE_NAME, __VA_ARGS__)

void __vdo_log_message(int priority, const char *module, const char *format, ...)
	__printf(3, 4);
#else /* not __KERNEL__ */
#if defined(TEST_INTERNAL) || defined(INTERNAL)
void uds_log_message(int priority, const char *format, ...);
#else /* neither TEST_INTERNAL nor INTERNAL */
void uds_log_message(int priority, const char *format, ...)
	__printf(2, 3);
#endif /* TEST_INTERNAL */
#endif /* __KERNEL__ */

#define vdo_log_debug(...) vdo_log_message(VDO_LOG_DEBUG, __VA_ARGS__)

#define vdo_log_info(...) vdo_log_message(VDO_LOG_INFO, __VA_ARGS__)

#define vdo_log_warning(...) vdo_log_message(VDO_LOG_WARNING, __VA_ARGS__)

#define vdo_log_error(...) vdo_log_message(VDO_LOG_ERR, __VA_ARGS__)

#define vdo_log_fatal(...) vdo_log_message(VDO_LOG_CRIT, __VA_ARGS__)

void vdo_pause_for_logger(void);
#ifndef __KERNEL__

void open_uds_logger(void);
#ifdef TEST_INTERNAL

void reinit_uds_logger(void);
#endif /* TEST_INTERNAL */
#endif /* __KERNEL__ */
#endif /* VDO_LOGGER_H */
