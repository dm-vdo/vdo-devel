// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "string-utils.h"

#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

#if !defined(__KERNEL__) || defined(TEST_INTERNAL)
/*
 * Allocate memory to contain a formatted string. The caller is responsible for
 * freeing the allcated memory.
 */
int uds_alloc_sprintf(const char *what, char **strp, const char *fmt, ...)
{
	va_list args;
	int result;
	int count;

	if (strp == NULL)
		return UDS_INVALID_ARGUMENT;

	va_start(args, fmt);
	count = vsnprintf(NULL, 0, fmt, args) + 1;
	va_end(args);
	result = UDS_ALLOCATE(count, char, what, strp);
	if (result == UDS_SUCCESS) {
		va_start(args, fmt);
		vsnprintf(*strp, count, fmt, args);
		va_end(args);
	}

	if ((result != UDS_SUCCESS) && (what != NULL))
		uds_log_error("cannot allocate %s", what);

	return result;
}

#endif /* (not __KERNEL) or TEST_INTERNAL */
#ifdef TEST_INTERNAL
/* Format a string into a fixed-size buffer, similar to snprintf. */
int uds_fixed_sprintf(char *buf,
		      size_t buf_size,
		      const char *fmt,
		      ...)
{
	va_list args;
	int n;

	if (buf == NULL)
		return UDS_INVALID_ARGUMENT;

	va_start(args, fmt);
	n = vsnprintf(buf, buf_size, fmt, args);
	va_end(args);

	if (n < 0)
		return uds_log_error_strerror(UDS_UNKNOWN_ERROR,
					      "%s: vsnprintf failed",
					      __func__);

	if ((size_t) n >= buf_size)
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "%s: string too long",
					      __func__);

	return UDS_SUCCESS;
}

#endif /* TEST_INTERNAL */
/* Append a formatted string to the end of a buffer. */
char *uds_append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
{
	va_list args;
	size_t n;

	va_start(args, fmt);
	n = vsnprintf(buffer, buf_end - buffer, fmt, args);
	if (n >= (size_t) (buf_end - buffer))
		buffer = buf_end;
	else
		buffer += n;
	va_end(args);

	return buffer;
}
