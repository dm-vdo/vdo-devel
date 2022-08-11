// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "permassert.h"

#include "errors.h"
#include "logger.h"

#ifndef __KERNEL__
#ifdef NDEBUG
#define DEBUGGING_OFF
#undef NDEBUG
#endif /* NDEBUG */

#include <assert.h>
#include <stdlib.h>
#include <strings.h>
#include <syslog.h>

#include "common.h"
#include "string-utils.h"
#include "uds-threads.h"

#ifdef DEBUGGING_OFF
static bool exit_on_assertion_failure = false;
#else /* not DEBUGGING_OFF */
static bool exit_on_assertion_failure = true;
#endif /* DEBUGGING_OFF */

static const char *EXIT_ON_ASSERTION_FAILURE_VARIABLE =
	"UDS_EXIT_ON_ASSERTION_FAILURE";

static atomic_t init_once = ATOMIC_INIT(0);
static struct mutex mutex = { .mutex = UDS_MUTEX_INITIALIZER };

static void initialize(void)
{
	uds_initialize_mutex(&mutex, !UDS_DO_ASSERTIONS);
	char *exit_on_assertion_failure_string =
		getenv(EXIT_ON_ASSERTION_FAILURE_VARIABLE);
	if (exit_on_assertion_failure_string != NULL) {
		exit_on_assertion_failure =
			(strcasecmp(exit_on_assertion_failure_string,
				    "true") == 0);
	}
}

bool set_exit_on_assertion_failure(bool should_exit)
{
	bool previous_setting;

	perform_once(&init_once, initialize);
	uds_lock_mutex(&mutex);
	previous_setting = exit_on_assertion_failure;
	exit_on_assertion_failure = should_exit;
	uds_unlock_mutex(&mutex);
	return previous_setting;
}

#endif /*  not __KERNEL__ */

int uds_assertion_failed(const char *expression_string,
			 const char *file_name,
			 int line_number,
			 const char *format,
			 ...)
{
	va_list args;

	va_start(args, format);

	uds_log_embedded_message(UDS_LOG_ERR,
				 UDS_LOGGING_MODULE_NAME,
				 "assertion \"",
				 format,
				 args,
				 "\" (%s) failed at %s:%d",
				 expression_string,
				 file_name,
				 line_number);
	uds_log_backtrace(UDS_LOG_ERR);

#ifndef __KERNEL__
	perform_once(&init_once, initialize);
	uds_lock_mutex(&mutex);
	if (exit_on_assertion_failure) {
		__assert_fail(expression_string,
			      file_name,
			      line_number,
			      __ASSERT_FUNCTION);
	}
	uds_unlock_mutex(&mutex);
#endif /* not __KERNEL__ */

	va_end(args);

	return UDS_ASSERTION_FAILED;
}
