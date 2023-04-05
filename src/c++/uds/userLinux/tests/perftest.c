/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <stdio.h>
#include <sys/stat.h>

#include "albtest.h"
#include "albtestCommon.h"
#include "fileUtils.h"
#include "logger.h"
#include "testPrototypes.h"

/**********************************************************************/
void albFlush(void)
{
  fflush(NULL);
}

/**********************************************************************/
void albPrint(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  putchar('\n');
  va_end(args);
}

/**********************************************************************/
static void createIndexFile(void)
{
  int fd;
  const char *path = getTestIndexName();
  int result = open_file(path, FU_CREATE_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to initialize index file: %s: %s", path,
         uds_string_error(result, errbuf, sizeof(errbuf)));
  }
  close_file(fd, NULL);
}

/**********************************************************************/
static void removeIndexFile(void)
{
  const char *path = getTestIndexName();
  int result = remove_file(path);
  if (result != UDS_SUCCESS) {
    char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to remove index file: %s: %s", path,
         uds_string_error(result, errbuf, sizeof(errbuf)));
  }
}

/**********************************************************************/
TestResult runTest(const CU_SuiteInfo *suite, const CU_TestInfo *test)
{
  TestResult result = {
    .name  = test->name,
    .tests = 1,
  };

  printf("  %s ", test->name);
  testSub(suite, test);
  result.failures = 0;

  if (result.failures > 0) {
    printf("FAILED\n");
  } else {
    printf("passed\n");
  }
  return result;
}

/**********************************************************************/
static void printFailuresToStderr(unsigned int indent, TestResult tr)
{
  if (tr.failures > 0) {
    if (tr.numSub > 0) {
      printf("%*s%s\n", indent, "", tr.name);
      for (unsigned int i = 0; i < tr.numSub; ++i) {
        printFailuresToStderr(indent + 2, tr.sub[i]);
      }
    } else {
      printf("%*s%s FAILED\n", indent, "", tr.name);
    }
  }
}

/**********************************************************************/
int main(int argc, const char **argv)
{
  // Make argc/argv available to the test.  Throw away argv[0].
  testArgc = argc - 1;
  testArgv = &argv[1];

  // Tests create files in global filespace.  Turn off private umask.
  umask(0);

  open_uds_logger();
  createIndexFile();
  TestResult result = runSuites(initializeModule());
  printFailuresToStderr(0, result);
  freeTestResults(&result);
  removeIndexFile();
  return 0;
}
