/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <stdio.h>
#include <sys/types.h>

/**
 * Fork a child process and add it to the list of managed processes.
 *
 * @return The pid of the child process
 **/
pid_t forkChild(void)
  __attribute__((__warn_unused_result__));

/**
 * Start a child process via forkChild() and open a pipe which allows reading
 * of the child's stdout. This is a replacement for popen().
 *
 * @param command The command for the child process to run
 * @param pidPtr  A pointer to hold the pid of the child process
 *
 * @return A pipe to the child's stdout.
 **/
FILE *openProcessPipe(const char *command, pid_t *pidPtr)
  __attribute__((__warn_unused_result__));

/**
 * Get the status of a managed child process. This function will block if
 * the child is still running.
 *
 * @param pid The pid of the child process to wait for
 *
 * @return The exit status of the child process
 **/
int getStatus(pid_t pid)
  __attribute__((__warn_unused_result__));

/**
 * Get the exit status of a managed child process and assert that it
 * matches a given expectation.
 *
 * @param pid            The pid of the child process to wait for
 * @param expectedStatus The expected exit status of the process
 **/
void expectStatus(pid_t pid, int status);

/**
 * Kill all children which were started via forkChild().
 **/
void killChildren(void);

#endif /* PROCESS_MANAGER_H */
