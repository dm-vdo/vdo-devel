/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <dirent.h>
#include <err.h>
#include <stdio.h>
#include <unistd.h>

#include "memory-alloc.h"
#include "resourceUsage.h"
#include "string-utils.h"

/**
 * Print a line from a /proc file
 *
 * @param path  Name of the /proc file
 * @param info  Name of the line of interest
 */
static void printProcLine(const char *path, const char *info)
{
  size_t infoLen = strlen(info);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    err(1, "failed to open %s", path);
  }
  char lineBuf[100];
  while (fgets(lineBuf, sizeof(lineBuf), f) != NULL) {
    if (strncmp(info, lineBuf, infoLen) == 0) {
      fputs(lineBuf, stdout);
    }
  }
  if (ferror(f)) {
    errx(1, "error reading from %s", path);
  }
  fclose(f);
}

/**********************************************************************/
static inline double tv2sec(struct timeval tv)
{
  return (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
}

/**********************************************************************/
/**
 * Thread statistics as gathered from /proc/\<id\>/stat or
 * /proc/\<id\>/task/\<id\>/stat.  See "man 5 proc" for details.
 **/
struct threadStatistics {
  char comm[16];                /* thread name */
  unsigned long usertime;       /* Clock ticks using CPU in user mode */
  unsigned long systime;        /* Clock ticks using CPU in system mode */
  int id;                       /* Thread id */
  ThreadStatistics *next;
};

/**********************************************************************/
static void addThreadStatistics(ThreadStatistics **tsList,
                                const ThreadStatistics *tsNew)
{
  // Allocate a new ThreadStatistics and copy the data into it
  ThreadStatistics *ts;
  if (uds_allocate(1, ThreadStatistics, __func__, &ts) == UDS_SUCCESS) {
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
void freeThreadStatistics(ThreadStatistics *ts)
{
  while (ts != NULL) {
    ThreadStatistics *tsNext = ts->next;
    uds_free(ts);
    ts = tsNext;
  }
}

/**********************************************************************/
ThreadStatistics *getThreadStatistics(void)
{
  ThreadStatistics ts;
  ThreadStatistics *tsList = NULL;
  // Get statistics for the whole process
  FILE *stream = fopen("/proc/self/stat", "r");
  if (stream != NULL) {
    if (4 == fscanf(stream, "%d (%16[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u"
                    " %*u %*u %lu %lu", &ts.id, ts.comm, &ts.usertime,
                    &ts.systime)) {
      strcpy(ts.comm, "*all*");
      ts.id = 0;
      addThreadStatistics(&tsList, &ts);
    }
    fclose(stream);
  }
  // Get statistics for each individual thread
  DIR *dir = opendir("/proc/self/task");
  if (dir != NULL) {
    struct dirent *de;
    while (true) {
      de = readdir(dir);
      if (de == NULL) {
	break;
      }
      if (de->d_name[0] == '.') {
        continue;
      }
      char path[32 + 256];
      snprintf(path, sizeof(path), "/proc/self/task/%s/stat", de->d_name);
      stream = fopen(path, "r");
      if (stream == NULL) {
        continue;
      }
      if (4 == fscanf(stream, "%d (%16[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u"
                      " %*u %*u %lu %lu", &ts.id, ts.comm, &ts.usertime,
                      &ts.systime)) {
        addThreadStatistics(&tsList, &ts);
      }
      fclose(stream);
    }
    closedir(dir);
  }
  return tsList;
}

/**********************************************************************/
void printResourceUsage(ResourceUsage *prev,
                        ResourceUsage *cur,
                        ktime_t elapsed)
{
  double elapsed_real = elapsed / 1.0e9;
  double elapsed_user = tv2sec(cur->ru_utime) - tv2sec(prev->ru_utime);
  double elapsed_sys  = tv2sec(cur->ru_stime) - tv2sec(prev->ru_stime);

  printf("Resource Usage: user_cpu=%.2f%% sys_cpu=%.2f%% total_cpu=%.2f%%"
         " inblock=%ld outblock=%ld nvcsw=%ld nivcsw=%ld minflt=%ld"
         " majflt=%ld\n",
         (elapsed_user / elapsed_real) * 100.0,
         (elapsed_sys / elapsed_real) * 100.0,
         ((elapsed_user + elapsed_sys) / elapsed_real) * 100.0,
         (cur->ru_inblock - prev->ru_inblock),
         (cur->ru_oublock - prev->ru_oublock),
         (cur->ru_nvcsw - prev->ru_nvcsw),
         (cur->ru_nivcsw - prev->ru_nivcsw),
         (cur->ru_minflt - prev->ru_minflt),
         (cur->ru_majflt - prev->ru_majflt));
}

/**********************************************************************/
void printThreadStatistics(ThreadStatistics *prev, ThreadStatistics *cur)
{
  double tps = sysconf(_SC_CLK_TCK);
  printf("Thread             User Time Sys Time Note\n");
  printf("================== ========= ======== ====\n");
  while ((prev != NULL) && (cur != NULL)) {
    if ((cur == NULL) || (prev->id < cur->id)) {
      printf("  %-35s gone\n", prev->comm);
      prev = prev->next;
    } else if ((prev == NULL) || (prev->id > cur->id)) {
      printf("  %-16.16s %9.3f %8.3f new\n", cur->comm, cur->usertime / tps,
             cur->systime / tps);
      cur = cur->next;
    } else {
      printf("  %-16.16s %9.3f %8.3f\n", cur->comm,
             (cur->usertime - prev->usertime) / tps,
             (cur->systime - prev->systime) / tps);
      prev = prev->next;
      cur = cur->next;
    }
  }
}

/**********************************************************************/
void printVmStuff(void)
{
  printProcLine("/proc/self/status", "VmHWM");
  printProcLine("/proc/self/status", "VmPeak");
  printProcLine("/proc/meminfo", "MemTotal");
}
