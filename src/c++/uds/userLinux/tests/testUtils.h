/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <sys/wait.h>

/**
 * Create a temporary file name not matching any existing file.
 *
 * @param what           a label to use in the filename and error messages
 *
 * @return               the open file descriptor
 **/
char *makeTempFileName(const char *what);

/**
 * Extract the exit status of a process from the wait() result.
 *
 * This is a more readable substitute for the WEXITSTATUS macro when
 * printed in assertion failure messages.
 *
 * @param waitResult      the result from one of the wait() family of calls
 *
 * @return   the exit status of the child process
 **/
static inline int extractExitStatus(int waitResult)
{
  return WEXITSTATUS(waitResult);
}

#endif /* TEST_UTILS_H */
