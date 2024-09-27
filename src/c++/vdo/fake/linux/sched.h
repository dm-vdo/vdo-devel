/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/sched.h.
 *
 * Copyright 2024 Red Hat
 *
 */

#ifndef LINUX_SCHED_H
#define LINUX_SCHED_H

/*
 * Task state bitmask. NOTE! These bits are also
 * encoded in fs/proc/array.c: get_task_state().
 *
 * We have two separate sets of flags: task->__state
 * is about runnability, while task->exit_state are
 * about the task exiting. Confusing, but this way
 * modifying one set can't modify the other one by
 * mistake.
 */

/* Used in tsk->__state: */
#define TASK_RUNNING                0x00000000
#define TASK_INTERRUPTIBLE          0x00000001
#define TASK_UNINTERRUPTIBLE        0x00000002
#define TASK_PARKED                 0x00000040

/* Convenience macros for the sake of wake_up(): */
#define TASK_NORMAL        (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)

enum {
  TASK_COMM_LEN = 16,
};

struct task_struct;

/**********************************************************************/
void io_schedule(void);

/* Modifications for VDO - used in mutexUtils.c */
void set_current_state(int state_value);

struct task_struct *getCurrentTaskStruct(void);

#define current (getCurrentTaskStruct())

#endif // LINUX_SCHED_H
