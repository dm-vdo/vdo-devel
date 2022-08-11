/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <sys/types.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <uds-threads.h>
#include <unistd.h>

#include "albtest.h"
#include "assertions.h"
#include "logger.h"
#include "memory-alloc.h"
#include "minisyslog.h"
#include "processManager.h"
#include "type-defs.h"

static char timeBuffer[24];

/**********************************************************************/
static void init(void)
{
  time_t startTime = time(NULL);
  CU_ASSERT_TRUE(startTime != (time_t)-1);
  srand(startTime);

  startTime--;
  struct tm *tm = localtime(&startTime);
  CU_ASSERT_TRUE(tm != NULL);
  size_t timeSize = strftime(timeBuffer, sizeof(timeBuffer), "%F %T", tm);
  CU_ASSERT_TRUE(timeSize < sizeof(timeBuffer));
}

/**********************************************************************/
static bool searchPipe(const char *pipe, const char *str)
{
  FILE *fp = openProcessPipe(pipe, NULL);

  char buf[256];
  bool found = false;
  regex_t regex;
  int result = regcomp(&regex, str, REG_NOSUB);
  CU_ASSERT_EQUAL(result, 0);
  while (fgets(buf, sizeof(buf), fp) != NULL) {
    if (regexec(&regex, buf, 0, NULL, 0) == 0) {
      found = true;
      break;
    }
  }
  regfree(&regex);
  fclose(fp);
  return found;
}

/**********************************************************************/
static void assertFound(const char *str)
{
  char *journalctlCommand;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf("journalCommand", &journalctlCommand,
                                       "sudo journalctl -a -S '%s'",
                                       timeBuffer));
  // ALB-2828 showed a delay in our logging making it to a syslog file.
  // ALB-2919 showed a delay longer than 3 seconds, so hunt longer.
  // FIXME: It is not clear that this is relevant now that we use journalctl.
  for (int delay = 0; delay < 12; delay++) {
    // Sleep a little to give syslog a chance.  Sleep a little longer each time
    // around the loop.
    if (delay > 0) {
      sleep(delay);
    }
    /* Search the journal log if not found already. */
    if (searchPipe(journalctlCommand, str)) {
      UDS_FREE(journalctlCommand);
      return;
    }
  }
  CU_FAIL("Couldn't find logged pattern \"%s\" in journald log", str);
}

/**********************************************************************/
static void simple(void)
{
  char *buf;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &buf, "foo simple %u",
                                       rand()));
  mini_syslog(UDS_LOG_ERR, "%s", buf);
  assertFound(buf);
  UDS_FREE(buf);
}

/**********************************************************************/
static void labeled(void)
{
  mini_openlog("foo", 0, LOG_USER);
  char *buf;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &buf,"foo labeled %u",
                                       rand()));
  mini_syslog(UDS_LOG_ERR, "%s", buf);
  char *line;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &line,
                                       "foo\\(\\[%d\\]\\)\\{0,1\\}: %s",
                                       getpid(), buf));
  UDS_FREE(buf);
  assertFound(line);
  UDS_FREE(line);
}

/**********************************************************************/
static void labeledPid(void)
{
  mini_openlog("foo", LOG_PID, LOG_USER);
  char *buf;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &buf,"foo labeledPid %u",
                                       rand()));
  mini_syslog(UDS_LOG_ERR, "%s", buf);
  char *line;
  char tname[16];
  uds_get_thread_name(tname);
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &line,
                                       "foo\\[%u\\]: ERROR  (%s/%d) %s",
                                       getpid(), tname, uds_get_thread_id(),
                                       buf));
  UDS_FREE(buf);
  assertFound(line);
  UDS_FREE(line);
}

/**********************************************************************/
static void unloadedName(void)
{
  // Verify that the identity string sticks even if the string that it
  // was initialized from is unmapped, as might happen when a shared
  // object is unloaded.
  int pagesize = sysconf(_SC_PAGE_SIZE);
  char *mem = mmap(NULL, pagesize, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  CU_ASSERT_NOT_EQUAL(mem, MAP_FAILED);
  static const char identity[] = "Minisyslog_t1:unloadedName";
  strncpy(mem, identity, pagesize);
  mem[pagesize - 1] = '\0';
  mini_closelog();
  mini_openlog(mem, LOG_PID, LOG_USER);
  char *test1;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &test1, "test1 %u", rand()));
  mini_syslog(UDS_LOG_ERR, "%s", test1);
  // Simulate unloading a shared object...
  UDS_ASSERT_SYSTEM_CALL(munmap(mem, pagesize));
  // ...followed by some action that logs a message.
  char *test2;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &test2, "test2 %u", rand()));
  mini_syslog(UDS_LOG_ERR, "%s", test2);
  char *buf;
  char tname[16];
  uds_get_thread_name(tname);
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &buf,
                                       "%s\\[%u\\]: ERROR  (%s/%d) %s",
                                       identity, getpid(), tname,
                                       uds_get_thread_id(), test1));
  assertFound(buf);
  UDS_FREE(buf);
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &buf,
                                       "%s\\[%u\\]: ERROR  (%s/%d) %s",
                                       identity, getpid(), tname,
                                       uds_get_thread_id(), test2));
  assertFound(buf);
  UDS_FREE(buf);
  UDS_FREE(test1);
  UDS_FREE(test2);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"simple",       simple},
  {"labeled",      labeled},
  {"labeledPid",   labeledPid},
  {"unloadedName", unloadedName},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "LinuxMinisyslog_t1",
  .initializer = init,
  .tests       = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
