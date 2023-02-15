/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef DIRECTORY_UTILS_H
#define DIRECTORY_UTILS_H 1

#include <dirent.h>
#include <linux/compiler_attributes.h>
#include <sys/stat.h>

/**
 * Determine whether or not the given path is a directory
 *
 * @param path      The path to the possible directory
 * @param directory A pointer to a bool which will be set to true if
 *                  the specified path is a directory
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check is_directory(const char *path, bool *directory);

/**
 * Wrap the opendir(2) system call.
 *
 * @param name           The name of the directory to open
 * @param directory_type The type of directory (for error reporting)
 * @param context        The calling context (for logging)
 * @param directory_ptr  A pointer to hold the new file offset
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check open_directory(const char *name,
				const char *directory_type,
				const char *context,
				DIR **directory_ptr);

/**
 * Wrap the closedir(2) system call.
 *
 * @param dir     The directory to close
 * @param context The calling context (for logging)
 *
 * @return UDS_SUCCESS or an error code
 **/
int close_directory(DIR *dir, const char *context);

#endif /* DIRECTORY_UTILS_H */
