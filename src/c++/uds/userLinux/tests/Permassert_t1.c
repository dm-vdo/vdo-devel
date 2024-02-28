/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <stdlib.h>
#include <unistd.h>

#include "albtest.h"
#include "assertions.h"
#include "logger.h"
#include "permassert.h"
#include "testUtils.h"

static char *logLevel;
static char *logFile;
static char *originalLogFile;
static bool  originalExitOnAssertionFailure;

/**********************************************************************/
static void init(void)
{
  originalExitOnAssertionFailure = set_exit_on_assertion_failure(false);
  logLevel = getenv("UDS_LOG_LEVEL");
  if (logLevel != NULL) {
    unsetenv("UDS_LOG_LEVEL");
  }

  originalLogFile = getenv("UDS_LOGFILE");

  srand(time(NULL));
  logFile = makeTempFileName("udsLogger");
  setenv("UDS_LOGFILE", logFile, 1);

  reinit_uds_logger();
}

/**********************************************************************/
static void fini(void)
{
  if (originalLogFile != NULL) {
    setenv("UDS_LOGFILE", originalLogFile, 1);
  } else {
    unsetenv("UDS_LOGFILE");
  }

  if (logFile != NULL) {
    unlink(logFile);
    free(logFile);
  }

  if (logLevel != NULL) {
    setenv("UDS_LOG_LEVEL", logLevel, 1);
  } else {
    unsetenv("UDS_LOG_LEVEL");
  }

  reinit_uds_logger();

  set_exit_on_assertion_failure(originalExitOnAssertionFailure);
}

/**********************************************************************/
static void checkFor(const char *str, bool wanted)
{
  FILE *fp = fopen(logFile, "r");
  if (wanted) {
    CU_ASSERT_PTR_NOT_NULL(fp);
  } else if (fp == NULL) {
    return;
  }

  char buf[256];
  memset(buf, 0, sizeof(buf));
  bool found = false;
  while (!found && (fgets(buf, 256, fp) != NULL)) {
    found = (strstr(buf, str) != NULL);
  }
  fclose(fp);

  CU_ASSERT_EQUAL(found, wanted);
}

/**********************************************************************/
static void checkFound(const char *str)
{
  checkFor(str, true);
}

/**********************************************************************/
static void checkNotFound(const char *str)
{
  checkFor(str, false);
}

/**********************************************************************/
static void testAssertionSuccess(void)
{
  UDS_ASSERT_SUCCESS(VDO_ASSERT(true, "true"));
  checkNotFound("assertion");
  checkNotFound("[Call Trace:]");
}

/**********************************************************************/
static void testAssertionFailure(void)
{
  CU_ASSERT_EQUAL(VDO_ASSERT(false, "false"), UDS_ASSERTION_FAILED);
  checkFound("assertion \"false\" (0) failed at");
  checkFound("Permassert_t1.c:");
  checkFound("[Call Trace:]");
  checkFound("maps file");
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"successful assertion",            testAssertionSuccess                 },
  {"failed assertion",                testAssertionFailure                 },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Permassert_t1",
  .initializerWithArguments = NULL,
  .initializer              = init,
  .cleaner                  = fini,
  .tests                    = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
