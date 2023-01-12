/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef DIRECTORY_READER_H
#define DIRECTORY_READER_H

#include <dirent.h>

#include "common.h"

/*
 * A function which processes directory entries. It's arguments are a
 * directory entry, the name of the directory being read, a context,
 * and a pointer to hold an error code. The function returns true if
 * the reader should stop reading the directory.
 */
typedef bool directory_entry_processor_t(struct dirent *entry,
					 const char *directory,
					 void *context,
					 int *result);

/**
 * Read a directory, passing each entry to a supplied reader function.
 *
 * @param path             The path of the directory to read
 * @param directory_type   The type of directory (for error reporting)
 * @param entry_processor  The function to call for each entry in the directory
 * @param context          The context to pass to the entry processor
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check read_directory(const char *path,
				const char *directory_type,
				directory_entry_processor_t *entry_processor,
				void *context);

#endif /* DIRECTORY_READER_H */
