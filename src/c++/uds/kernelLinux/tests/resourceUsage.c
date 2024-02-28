// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/task_io_accounting_ops.h>

#include "logger.h"
#include "memory-alloc.h"
#include "resourceUsage.h"
#include "thread-utils.h"

/**
 * Thread statistics as gathered from task_struct
 **/
struct threadStatistics {
  char comm[TASK_COMM_LEN];  /* thread name (may be unterminated) */
  unsigned long cputime;     /* Nanoseconds using CPU */
  unsigned long inblock;     /* Sectors read */
  unsigned long outblock;    /* Sectors written */
  pid_t id;                  /* Thread id */
  ThreadStatistics *next;
};

/**********************************************************************/
static void addThreadStatistics(ThreadStatistics **tsList,
                                const ThreadStatistics *tsNew)
{
  // Allocate a new ThreadStatistics and copy the data into it
  ThreadStatistics *ts;
  if (vdo_allocate(1, ThreadStatistics, __func__, &ts) == VDO_SUCCESS) {
    *ts = *tsNew;
    // Insert the new one into the list, sorted by id
    while ((*tsList != NULL) && (ts->id > (*tsList)->id)) {
      tsList = &(*tsList)->next;
    }
    ts->next = *tsList;
    *tsList = ts;
  }
}

/**********************************************************************/
static void addOneThread(void *arg, struct task_struct *task)
{
  ThreadStatistics ts = {
    .cputime  = task->se.sum_exec_runtime,
    .id       = task->pid,
    .inblock  = task_io_get_inblock(task) + task->signal->inblock,
    .outblock = task_io_get_oublock(task) + task->signal->oublock,
  };
  memcpy(ts.comm, task->comm, TASK_COMM_LEN);
  addThreadStatistics(arg, &ts);
}

/**********************************************************************/
void freeThreadStatistics(ThreadStatistics *ts)
{
  while (ts != NULL) {
    ThreadStatistics *tsNext = ts->next;
    vdo_free(ts);
    ts = tsNext;
  }
}

/**********************************************************************/
ThreadStatistics *getThreadStatistics(void)
{
  ThreadStatistics *tsList = NULL;
  uds_apply_to_threads(addOneThread, &tsList);
  return tsList;
}

/**********************************************************************/
void printThreadStatistics(ThreadStatistics *prev, ThreadStatistics *cur)
{
  const unsigned long MILLION = 1000 * 1000;
  const unsigned long BILLION = 1000 * 1000 * 1000;
  uds_log_info("Thread           CPUTime    Inblock Outblock Note");
  uds_log_info("================ ========== ======= ======== ====");
  while ((prev != NULL) && (cur != NULL)) {
    if ((cur == NULL) || (prev->id < cur->id)) {
      uds_log_info("  %-45.*s gone", TASK_COMM_LEN, prev->comm);
      prev = prev->next;
    } else if ((prev == NULL) || (prev->id > cur->id)) {
      uds_log_info("%-16.*s %3lu.%06lu %7lu %8lu new",
                   TASK_COMM_LEN, cur->comm,
                   cur->cputime / BILLION, cur->cputime / 1000 % MILLION,
                   cur->inblock, cur->outblock);
      cur = cur->next;
    } else {
      uds_log_info("%-16.*s %3lu.%06lu %7lu %8lu",
                   TASK_COMM_LEN, cur->comm,
                   (cur->cputime - prev->cputime) / BILLION,
                   (cur->cputime - prev->cputime) / 1000 % MILLION,
                   cur->inblock - prev->inblock, cur->outblock - prev->outblock);
      prev = prev->next;
      cur = cur->next;
    }
  }
}
