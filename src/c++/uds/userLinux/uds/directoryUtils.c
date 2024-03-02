// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "directoryUtils.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include "directoryReader.h"
#include "errors.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "string-utils.h"
#include "syscalls.h"

/**********************************************************************/
int is_directory(const char *path, bool *directory)
{
	struct stat stat_buf;
	int result = logging_stat_missing_ok(path, &stat_buf, __func__);
	if (result == UDS_SUCCESS)
		*directory = (bool) S_ISDIR(stat_buf.st_mode);
	return result;
}

/**********************************************************************/
int open_directory(const char *name,
		   const char *directory_type,
		   const char *context,
		   DIR **directory_ptr)
{
	DIR *directory = opendir(name);
	if (directory == NULL)
		return vdo_log_error_strerror(errno,
					      "%s failed in %s on %s directory %s",
					      __func__,
					      context,
					      directory_type,
					      name);

	*directory_ptr = directory;
	return UDS_SUCCESS;
}

/**********************************************************************/
int close_directory(DIR *dir, const char *context)
{
	return check_system_call(closedir(dir), __func__, context);
}
