/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "directoryReader.h"

#include <unistd.h>

#include "directoryUtils.h"
#include "errors.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"

/**********************************************************************/
int read_directory(const char *path,
		   const char *directory_type,
		   directory_entry_processor_t entry_processor,
		   void *context)
{
	DIR *directory;
	int result =
		open_directory(path, directory_type, __func__, &directory);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (;;) {
		struct dirent *entry;
		errno = 0;
		entry = readdir(directory);
		if (entry == NULL) {
			result = errno;
			break;
		}
		if ((strcmp(entry->d_name, ".") == 0) ||
		    (strcmp(entry->d_name, "..") == 0)) {
			continue;
		}
		if (((*entry_processor)(entry, path, context, &result)) ||
		    (result != UDS_SUCCESS)) {
			break;
		}
	}

	close_directory(directory, __func__);
	return result;
}
