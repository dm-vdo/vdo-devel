/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */

#ifndef LINUX_DELAY_H
#define LINUX_DELAY_H

/**
 * Sleep safely, even with waitqueue interruptions. For unit tests, doesn't
 * actually sleep.
 *
 * @param usecs  The number of microseconds we won't sleep for
 **/
static inline void fsleep(unsigned int usecs)
{
  return;
}

#endif // LINUX_DELAY_H
