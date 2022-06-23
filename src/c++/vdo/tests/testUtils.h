/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <sys/time.h>
#include <sys/wait.h>

/**
 * Return a timeval in usecs.
 **/
static inline long tv2usec(struct timeval tv)
{
  return (tv.tv_sec * 1000000L + tv.tv_usec);
}

/**
 * Get the directory in which tests reside.
 *
 * @return the test directory
 **/
const char *getTestDirectory(void)
  __attribute__((__warn_unused_result__));

/**
 * Set the directory in which tests reside.
 *
 * @param directory  The test directory
 **/
void setTestDirectory(const char *directory);

#endif /* TEST_UTILS_H */
