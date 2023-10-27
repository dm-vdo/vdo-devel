// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/kobject.h>
#include <linux/module.h>

#include "albtest.h"
#include "albtestCommon.h"
#include "logger.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "uds-threads.h"

typedef struct suiteState {
  struct kobject      kobjSuite;
  struct suiteState  *next;
  const char         *name;
  const CU_SuiteInfo *suite;
  TestResult          result;
  bool                resultAvailable;
} SuiteState;

typedef ssize_t SuiteShow(SuiteState *ss, char *buf);
typedef ssize_t SuiteStore(SuiteState *ss, const char *value, size_t count);

typedef struct suiteAttr {
  struct attribute attr;
  SuiteShow       *show;
  SuiteStore      *store;
} SuiteAttr;

typedef struct {
  struct attribute attr;
  const char      *parameterName;
} ConfigAttr;

static struct {
  struct kobject  kobj;
  SuiteState     *suites;
} moduleState;

/**********************************************************************/
void albPrint(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  /*
   * Formatting a message will allocate a buffer, which may or may not call the
   * scheduler and give other tasks the opportunity to run.
   *
   * In the interest of not triggering kernel warnings, we'll ensure that, if
   * we're taking the time to print things out, we'll always give other tasks
   * the opportunity to run. On a machine reserved for running our tests, there
   * shouldn't be much else running anyway.
   */
  cond_resched();
  uds_log_embedded_message(UDS_LOG_INFO, THIS_MODULE->name, NULL,
                           format, args, "%s", "");
  va_end(args);
}

/**********************************************************************/
void albFlush(void)
{
}

/**********************************************************************/
static int sprintName(char         *buf,
                      unsigned int  indent,
                      const char   *name,
                      unsigned int  failures,
                      unsigned int  skips)
{
  return sprintf(buf, "%*s%s (%u failed, %u skipped)\n",
                 indent, "", name, failures, skips);
}

/**********************************************************************/
static int sprintTestResult(char         *buf,
                            unsigned int  indent,
                            const char   *name,
                            const char   *result)
{
  unsigned int len = strlen(name);
  unsigned int col = 60;

  if (len + indent + 2 > col) {
    return sprintf(buf, "%*s%s\n%*s%s\n", indent, "", name, col, "", result);
  } else {
    return sprintf(buf, "%*s%s%*s%s\n", indent, "",
                   name, col - len - indent, "", result);
  }
}

/**********************************************************************/
static int sprintSummary(char *buf, unsigned int indent, TestResult tr)
{
  int written = 0;
  if (tr.numSub > 0) {
    written += sprintName(buf + written, indent,
                          tr.name, tr.failures, tr.skips);
    unsigned int i;
    for (i = 0; i < tr.numSub; ++i) {
      written += sprintSummary(buf + written, indent + 2, tr.sub[i]);
    }
  } else if (tr.failures) {
    written += sprintTestResult(buf + written, indent, tr.name, "FAILED");
  } else if (tr.skips) {
    written += sprintTestResult(buf + written, indent, tr.name, "(skipped)");
  } else {
    written += sprintTestResult(buf + written, indent, tr.name, "passed");
  }
  return written;
}

/**********************************************************************/
static int sprintElapsed(char *buf, unsigned int indent, TestResult tr)
{
  int written = 0;
  char *elapsed;
  if (rel_time_to_string(&elapsed, tr.elapsed) == UDS_SUCCESS) {
    written += sprintf(buf, "%*s%s %s\n", indent, "", tr.name, elapsed);
    uds_free(elapsed);
  }
  unsigned int i;
  for (i = 0; i < tr.numSub; ++i) {
    written += sprintElapsed(buf + written, indent + 2, tr.sub[i]);
  }
  return written;
}

/**********************************************************************/
static int parseArgs(const char   *buf,
                     size_t        length,
                     int          *argcPtr,
                     const char ***argvPtr,
                     char        **argBufPtr)
{
  char *argBuf;
  int result = UDS_ALLOCATE(length + 1, char, "argument list", &argBuf);
  if (result != UDS_SUCCESS) {
    return -ENOMEM;
  }
  int argc = 0;
  // XXX Do we need to process any escapes or quotes??
  int i;
  for (i = 0; i < length; i++) {
    if (buf[i] != '\n' && buf[i] != ' ' && buf[i] != '\000') {
      argBuf[i] = buf[i];
      if (i == length - 1) {
        ++argc;
      }
    } else {
      argBuf[i] = '\000';
      if (i > 0) {
        ++argc;
      }
    }
  }
  argBuf[length] = '\000';
  const char **argv;
  result = UDS_ALLOCATE(argc, char *, "argv", &argv);
  if (result != UDS_SUCCESS) {
    uds_free(argBuf);
    return -ENOMEM;
  }
  int argIndex = 0;
  char *arg = argBuf;
  for (i = 0; (i < length + 1) && (argIndex < argc); i++) {
    if (argBuf[i] == '\000') {
      argv[argIndex++] = arg;
      arg = argBuf + i + 1;
      if (arg < argBuf + length) {
      }
    }
  }

  uds_log_debug("storing %d args", argc);
  for (i = 0; i < argc; i++) {
    uds_log_debug("argv[%d] = %s", i, argv[i]);
  }

  *argcPtr   = argc;
  *argvPtr   = argv;
  *argBufPtr = argBuf;
  return 0;
}

/**********************************************************************/
static void suiteRelease(struct kobject *object)
{
  return;
}

/**********************************************************************/
static ssize_t suiteShow(struct kobject   *kobj,
                         struct attribute *attr,
                         char             *buf)
{
  SuiteAttr *suiteAttr = container_of(attr, SuiteAttr, attr);
  if (suiteAttr->show != NULL) {
    SuiteState *suiteState = container_of(kobj, SuiteState, kobjSuite);
    return suiteAttr->show(suiteState, buf);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t suiteStore(struct kobject   *kobj,
                          struct attribute *attr,
                          const char       *buf,
                          size_t            length)
{
  SuiteAttr *suiteAttr = container_of(attr, SuiteAttr, attr);
  if (suiteAttr->store != NULL) {
    SuiteState *suiteState = container_of(kobj, SuiteState, kobjSuite);
    return suiteAttr->store(suiteState, buf, length);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t showRun(SuiteState *ss, char *buf)
{
  buf[0] = '\000';
  return 1;
}

/**********************************************************************/
static ssize_t storeRun(SuiteState *ss, const char *buf, size_t length)
{
  char *argBuf = NULL;
  int result = parseArgs(buf, length, &testArgc, &testArgv, &argBuf);
  if (result != 0) {
    return result;
  }
  // Free the results of any previous run
  freeTestResults(&ss->result);
  ss->result = runSuites(ss->suite);
  ss->resultAvailable = true;
  if (testArgv != NULL) {
    uds_free(testArgv);
    testArgv = NULL;
  }
  uds_free(argBuf);
  return length;
}

/**********************************************************************/
static ssize_t showResults(SuiteState *ss, char *buf)
{
  if (!ss->resultAvailable) {
    return -EINVAL;
  }
  int written = sprintSummary(buf, 0, ss->result);
  return written;
}

/**********************************************************************/
static ssize_t showElapsed(SuiteState *ss, char *buf)
{
  if (!ss->resultAvailable) {
    return -EINVAL;
  }
  int written = sprintElapsed(buf, 0, ss->result);
  return written;
}

/**********************************************************************/
static ssize_t showTests(SuiteState *ss, char *buf)
{
  sprintf(buf, "%u\n", ss->result.tests);
  return strlen(buf);
}

/**********************************************************************/
static ssize_t showFailed(SuiteState *ss, char *buf)
{
  sprintf(buf, "%u\n", ss->result.failures);
  return strlen(buf);
}

/**********************************************************************/
static ssize_t showSkipped(SuiteState *ss, char *buf)
{
  sprintf(buf, "%u\n", ss->result.skips);
  return strlen(buf);
}

/**********************************************************************/

static struct sysfs_ops suiteOps = {
  .show  = suiteShow,
  .store = suiteStore,
};

static SuiteAttr suiteRunAttr = {
  .attr  = { .name = "run", .mode = 0200, },
  .show  = showRun,
  .store = storeRun,
};

static SuiteAttr suiteResultsAttr = {
  .attr  = { .name = "results", .mode = 0444, },
  .show  = showResults,
};

static SuiteAttr suiteElapsedAttr = {
  .attr  = { .name = "elapsed", .mode = 0444, },
  .show  = showElapsed,
};

static SuiteAttr suiteTestsAttr = {
  .attr  = { .name = "tests", .mode = 0444, },
  .show  = showTests,
};

static SuiteAttr suiteFailedAttr = {
  .attr  = { .name = "failed", .mode = 0444, },
  .show  = showFailed,
};

static SuiteAttr suiteSkippedAttr = {
  .attr  = { .name = "skipped", .mode = 0444, },
  .show  = showSkipped,
};

static struct attribute *suite_attrs[] = {
  &suiteRunAttr.attr,
  &suiteResultsAttr.attr,
  &suiteElapsedAttr.attr,
  &suiteTestsAttr.attr,
  &suiteFailedAttr.attr,
  &suiteSkippedAttr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(suite);

static struct kobj_type suiteObjectType = {
  .release        = suiteRelease,
  .sysfs_ops      = &suiteOps,
  .default_groups = suite_groups,
};

/**********************************************************************/
SuiteState *makeSuiteState(const CU_SuiteInfo *suite)
{
  SuiteState *ss;
  if (UDS_ALLOCATE(1, SuiteState, __func__, &ss) != UDS_SUCCESS) {
    return NULL;
  }
  ss->next = NULL;
  ss->name = suite->name;
  ss->suite = suite;
  ss->resultAvailable = false;
  kobject_init(&ss->kobjSuite, &suiteObjectType);
  int result = kobject_add(&ss->kobjSuite, &moduleState.kobj, ss->name);
  if (result != 0) {
    freeSuites(suite);
    uds_free(ss);
    return NULL;
  }
  return ss;
}

/**********************************************************************/
void freeSuiteState(SuiteState *ss)
{
  while (ss != NULL) {
    SuiteState *next = ss->next;
    kobject_put(&ss->kobjSuite);
    freeTestResults(&ss->result);
    freeSuites(ss->suite);
    uds_free(ss);
    ss = next;
  }
}

/**********************************************************************/
static void moduleRelease(struct kobject *object)
{
  return;
}

/**********************************************************************/
static ssize_t moduleShow(struct kobject   *kobj,
                          struct attribute *attr,
                          char             *buf)
{
  return -EINVAL;
}

/**********************************************************************/
static ssize_t moduleStore(struct kobject   *kobj,
                           struct attribute *attr,
                           const char       *buf,
                           size_t            length)
{
  return -EINVAL;
}

/**********************************************************************/

static struct sysfs_ops moduleOps = {
  .show  = moduleShow,
  .store = moduleStore,
};

static struct attribute *module_attrs[] = {
  NULL,
};
ATTRIBUTE_GROUPS(module);

static struct kobj_type moduleObjectType = {
  .release        = moduleRelease,
  .sysfs_ops      = &moduleOps,
  .default_groups = module_groups,
};

/**********************************************************************/
typedef struct {
  const CU_SuiteInfo *suite;
  const CU_TestInfo *test;
  TestResult *result;
} TestThreadData;

/**********************************************************************/
void testThread(void *argument)
{
  TestThreadData *ttd = argument;
  // Record a failure, in case this thread exits without returning from testSub
  ttd->result->failures += 1;
  testSub(ttd->suite, ttd->test);
  // Joy!  Not a failure
  ttd->result->failures -= 1;
}

/**********************************************************************/
TestResult runTest(const CU_SuiteInfo *suite, const CU_TestInfo *test)
{
  TestResult result = {
    .name  = test->name,
    .tests = 1,
  };
  TestThreadData ttd = {
    .suite  = suite,
    .test   = test,
    .result = &result,
  };
  albPrint("  %s...", test->name);
  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  struct thread *thread;
  int retval = uds_create_thread(testThread, &ttd, "zub:runtest", &thread);
  if (retval == UDS_SUCCESS) {
    uds_join_threads(thread);
  } else {
    uds_log_error_strerror(retval, "creating test thread");
    result.failures = 1;
  }
  result.elapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
  if (result.failures > 0) {
    printTestResult(2, test->name, "FAILED");
  } else if (result.skips > 0) {
    printTestResult(2, test->name, "(skipped)");
  } else {
    printTestResult(2, test->name, "passed");
  }
  return result;
}

/**********************************************************************/
static int __init albtestInit(void)
{
  uds_log_info("UDS tests starting");
  kobject_init(&moduleState.kobj, &moduleObjectType);
  int result = kobject_add(&moduleState.kobj, NULL, THIS_MODULE->name);
  if (result != 0) {
    return result;
  }
  SuiteState **nextSuiteState = &moduleState.suites;
  const CU_SuiteInfo *suites = initializeModule();
  const CU_SuiteInfo *suite;
  for (suite = suites; suite != NULL; suite = suite->next) {
    SuiteState *ss = makeSuiteState(copySuite(suite));
    if (ss != NULL) {
      *nextSuiteState = ss;
      nextSuiteState = &ss->next;
    }
  }
  SuiteState *ss;
  for (ss = moduleState.suites; ss != NULL; ss = ss->next) {
    if (ss->suite->mustRun) {
      ss->result = runSuites(ss->suite);
      ss->resultAvailable = true;
    }
  }
  return 0;
}

/**********************************************************************/
static void __exit albtestExit(void)
{
  freeSuiteState(moduleState.suites);
  kobject_put(&moduleState.kobj);
  uds_log_info("UDS tests exiting");
}

module_init(albtestInit);
module_exit(albtestExit);

MODULE_DESCRIPTION("UDS unit test");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
