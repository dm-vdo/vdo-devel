/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

#include "albtest.h"
#include "assertions.h"
#include "logger.h"
#include "testUtils.h"

static char *logFile;
static char *originalLogFile;
static char *originalLogLevel;

/**********************************************************************/
static void init(void)
{
  originalLogLevel = getenv("UDS_LOG_LEVEL");
  if (originalLogLevel != NULL) {
    unsetenv("UDS_LOG_LEVEL");
  }
  originalLogFile = getenv("UDS_LOG_FILE");
  if (originalLogFile != NULL) {
    unsetenv("UDS_LOG_FILE");
  }
  srand(time(NULL));
  logFile = makeTempFileName("udsLogger");
  setenv("UDS_LOGFILE", logFile, 1);
  reinit_uds_logger();
}

/**********************************************************************/
static void fini(void)
{
  // Put the environment back the way we found it
  unsetenv("UDS_LOGFILE");
  if (logFile != NULL) {
    unlink(logFile);
    free(logFile);
  }
  if (originalLogLevel != NULL) {
    setenv("UDS_LOG_LEVEL", originalLogLevel, 1);
  } else {
    unsetenv("UDS_LOG_LEVEL");
  }
  if (originalLogFile != NULL) {
    setenv("UDS_LOG_FILE", originalLogFile, 1);
  } else {
    unsetenv("UDS_LOG_FILE");
  }
  reinit_uds_logger();
}

/**********************************************************************/
static bool checkFor(const char *str, bool wanted)
{
  FILE *fp = fopen(logFile, "r");
  CU_ASSERT_PTR_NOT_NULL(fp);

  char buf[256];
  memset(buf, 0, sizeof(buf));
  int itemsRead = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  CU_ASSERT_TRUE(itemsRead >= 0);

  char *tmp = strstr(buf, str);
  return (tmp != NULL) == wanted;
}

/**********************************************************************/
static bool checkFound(const char *str)
{
  return checkFor(str, true);
}

/**********************************************************************/
static void testInfo(void)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "foo <%u>", rand());
  uds_log_info("blah %s", buf);
  CU_ASSERT_TRUE(checkFound(buf));
  CU_ASSERT_TRUE(checkFound("INFO"));
}

/**********************************************************************/
static void testNotice(void)
{
  char buf[128];
  snprintf(buf, sizeof(buf), "foo <%u>", rand());
  uds_log_notice("blah %s", buf);
  CU_ASSERT_TRUE(checkFound(buf));
  CU_ASSERT_TRUE(checkFound("NOTICE"));
}

/**********************************************************************/
static void testFiltering(void)
{
  setenv("UDS_LOG_LEVEL", "WARNING", 1);
  reinit_uds_logger();
  char buf[128];
  snprintf(buf, sizeof(buf), "foo <%u>", rand());
  uds_log_info("blah %s", buf);
  CU_ASSERT_FALSE(checkFound(buf));
  CU_ASSERT_FALSE(checkFound("INFO"));
  uds_log_warning("blah %s", buf);
  CU_ASSERT_TRUE(checkFound(buf));
  CU_ASSERT_TRUE(checkFound("WARN"));
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"info",          testInfo   },
  {"notice",        testNotice },
  {"testFiltering", testFiltering },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "Logger_t1",
  .initializer = init,
  .cleaner     = fini,
  .tests       = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
