/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_TIMER_H
#define TEST_TIMER_H

#include "types.h"

unsigned long getNextTimeout(void);
bool fireTimers(unsigned long at);

#endif // TEST_TIMER_H

