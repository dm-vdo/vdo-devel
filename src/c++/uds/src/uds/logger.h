/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef LOGGER_H
#define LOGGER_H 1

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/ratelimit.h>
#else
#include <stdarg.h>
#include "minisyslog.h"
#endif

#ifdef __KERNEL__
#define UDS_LOG_EMERG 0
#define UDS_LOG_ALERT 1
#define UDS_LOG_CRIT 2
#define UDS_LOG_ERR 3
#define UDS_LOG_WARNING 4
#define UDS_LOG_NOTICE 5
#define UDS_LOG_INFO 6
#define UDS_LOG_DEBUG 7
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
#if defined(MODULE)
#define UDS_LOGGING_MODULE_NAME THIS_MODULE->name
#else /* compiled into the kernel */
#define UDS_LOGGING_MODULE_NAME "vdo"
#endif
#else /* userspace */
#define UDS_LOGGING_MODULE_NAME "vdo"
#endif /* __KERNEL__ */

/* Apply a rate limiter to a log method call. */
#ifdef __KERNEL__
#define uds_log_ratelimit(log_fn, ...)                                    \
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

int get_uds_log_level(void);

void set_uds_log_level(int new_log_level);

int uds_log_string_to_priority(const char *string);

const char *uds_log_priority_to_string(int priority);

void uds_log_embedded_message(int priority,
			      const char *module,
			      const char *prefix,
			      const char *fmt1,
			      va_list args1,
			      const char *fmt2,
			      ...)
	__printf(4, 0) __printf(6, 7);

void uds_log_backtrace(int priority);

/* All log functions will preserve the caller's value of errno. */

#define uds_log_strerror(priority, errnum, ...)     \
	__uds_log_strerror(priority,                \
			   errnum,                  \
			   UDS_LOGGING_MODULE_NAME, \
			   __VA_ARGS__)

int __uds_log_strerror(int priority,
		       int errnum,
		       const char *module,
		       const char *format,
		       ...)
	__printf(4, 5);

int uds_vlog_strerror(int priority,
		      int errnum,
		      const char *module,
		      const char *format,
		      va_list args)
	__printf(4, 0);

/* Log an error prefixed with the string associated with the errnum. */
#define uds_log_error_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_ERR, errnum, __VA_ARGS__);

#define uds_log_debug_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_DEBUG, errnum, __VA_ARGS__);

#define uds_log_info_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_INFO, errnum, __VA_ARGS__);

#define uds_log_notice_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_NOTICE, errnum, __VA_ARGS__);

#define uds_log_warning_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_WARNING, errnum, __VA_ARGS__);

#define uds_log_fatal_strerror(errnum, ...) \
	uds_log_strerror(UDS_LOG_CRIT, errnum, __VA_ARGS__);

#ifdef __KERNEL__
#define uds_log_message(priority, ...) \
	__uds_log_message(priority, UDS_LOGGING_MODULE_NAME, __VA_ARGS__)

void __uds_log_message(int priority,
		       const char *module,
		       const char *format,
		       ...)
	__printf(3, 4);
#else
void uds_log_message(int priority, const char *format, ...)
	__printf(2, 3);
#endif /* __KERNEL__ */

#define uds_log_debug(...) uds_log_message(UDS_LOG_DEBUG, __VA_ARGS__)

#define uds_log_info(...) uds_log_message(UDS_LOG_INFO, __VA_ARGS__)

#define uds_log_notice(...) uds_log_message(UDS_LOG_NOTICE, __VA_ARGS__)

#define uds_log_warning(...) uds_log_message(UDS_LOG_WARNING, __VA_ARGS__)

#define uds_log_error(...) uds_log_message(UDS_LOG_ERR, __VA_ARGS__)

#define uds_log_fatal(...) uds_log_message(UDS_LOG_CRIT, __VA_ARGS__)

void uds_pause_for_logger(void);
#ifndef __KERNEL__

void open_uds_logger(void);
#ifdef TEST_INTERNAL

void reinit_uds_logger(void);
#endif /* TEST_INTERNAL */
#endif /* __KERNEL__ */
#endif /* LOGGER_H */
