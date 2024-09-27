/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testTimer.h"

#include <linux/timer.h>

#include <linux/jiffies.h>
#include <linux/list.h>

#include "mutexUtils.h"

// Mocks of timer.h and jiffies.h

unsigned long unitTestJiffies;

static LIST_HEAD(timers);

/**
 * Get the current mock jiffies, and increment for the next call.
 *
 * Implements LockedMethod
 **/
static bool getUnitTestJiffiesLocked(void *context)
{
  *((unsigned long *) context) = unitTestJiffies++;
  return false;
}

/**********************************************************************/
unsigned long getUnitTestJiffies(void)
{
  unsigned long result;

  runLocked(getUnitTestJiffiesLocked, &result);
  return result;
}

/**********************************************************************/
void __init_timer(struct timer_list *timer,
                  void (*func)(struct timer_list *),
                  unsigned int flags)
{
  INIT_LIST_HEAD(&timer->entry);
  timer->function = func;
  timer->flags = flags;
}

/**********************************************************************/
int mod_timer(struct timer_list *timer, unsigned long expires)
{
  lockMutex();
  int result = !list_empty(&timer->entry);
  list_del_init(&timer->entry);
  timer->expires = expires;
  list_add_tail(&timer->entry, &timers);
  unlockMutex();
  return result;
}

/**********************************************************************/
int del_timer_sync(struct timer_list *timer)
{
  int result;

  lockMutex();
  result = list_empty(&timer->entry);
  list_del_init(&timer->entry);
  unlockMutex();

  return result;
}

/**********************************************************************/
unsigned long getNextTimeout(void)
{
  unsigned long result = ULONG_MAX;

  lockMutex();
  struct timer_list *timer;
  list_for_each_entry(timer, &timers, entry) {
    result = min(result, timer->expires);
  }
  unlockMutex();

  return result;
}

/**********************************************************************/
bool fireTimers(unsigned long at)
{
  bool fired = false;

  lockMutex();
  if (at > unitTestJiffies) {
    unitTestJiffies = at;
  }

  struct timer_list *timer, *tmp;
  list_for_each_entry_safe(timer, tmp, &timers, entry) {
    if (timer->expires <= unitTestJiffies) {
      list_del_init(&timer->entry);
      timer->function(timer);
      fired = true;
    }
  }
  unlockMutex();

  return fired;
}
