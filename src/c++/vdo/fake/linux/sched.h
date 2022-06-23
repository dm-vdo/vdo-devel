/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test requirements from linux/sched.h.
 *
 * $Id$
 */
#ifndef LINUX_SCHED_H
#define LINUX_SCHED_H

enum {
  TASK_RUNNING,
  TASK_RUNNABLE,
  TASK_UNINTERRUPTIBLE,
  TASK_COMM_LEN = 16,
};

struct task_struct;

/**********************************************************************/
void io_schedule(void);

/**********************************************************************/
int wake_up_process(struct task_struct *p);

/**********************************************************************/
void set_current_state(int state_value);

/**********************************************************************/
struct task_struct *getCurrentTaskStruct(void);

#define current (getCurrentTaskStruct())

#endif // LINUX_SCHED_H
