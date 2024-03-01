/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <err.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "assertions.h"
#include "directoryUtils.h"
#include "fileUtils.h"
#include "logger.h"
#include "modloader.h"
#include "processManager.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "testUtils.h"
#include "thread-utils.h"

#include "dump.h"
#include "vdoTestBase.h"

enum {
  SINGLE_SUITE_MODULE = 1,
  MULTI_SUITE_MODULE  = 2,
  TEST_DIRECTORY_INIT = 3,
};

typedef struct testResult {
  const char        *name;
  unsigned int       tests;
  unsigned int       errors;
  unsigned int       failures;
  double             elapsed;
  unsigned int       numSub;
  struct testResult *sub;
} TestResult;

static CU_SuiteInfo nullSuite = CU_SUITE_INFO_NULL;

static const char usageString[] =
  " [--help]"
  " [--xml=FILENAME] [--repeat[=N]] [--no-unload] [--seed=SEED]"
  " [--no-fork] [--elapsed] [--test-directory=DIRECTORY] [--timeout=SECONDS]"
  " [pattern ...] [-- <test-specific options>]";

static const char helpString[] =
  "vdotest - run unit tests\n"
  "\n"
  "SYNOPSIS\n"
  "  vdotest [options] [pattern ...] [-- testoptions]\n"
  "\n"
  "DESCRIPTION\n"
  "  vdotest runs the test or tests that match [pattern ...] which is\n"
  "  a list of shell style wildcard patterns.  The default if no pattern is\n"
  "  given is '*_t[0-9]*.so'.  Command line options following the '--' are\n"
  "  passed directly to the initialization methods of the tests.\n"
  "\n"
  "OPTIONS\n"
  "\n"
  "    --help\n"
  "       Print this help message and exit\n"
  "\n"
  "    --xml=FILENAME\n"
  "       Output results as XML to file FILENAME\n"
  "\n"
  "    --repeat[=N]\n"
  "       Run the test[s] N times, or forever if N is not given\n"
  "\n"
  "    --no-unload\n"
  "       Do not unload test objects at the end of the run\n"
  "\n"
  "    --seed=SEED\n"
  "       Use SEED to seed the pseudo-random number generator\n"
  "\n"
  "    --no-fork\n"
  "       Do not fork a process for each test; instead run each test\n"
  "       in the main process\n"
  "\n"
  "    --elapsed\n"
  "       Print the elapsed time of each test\n"
  "\n"
  "    --test-directory=DIRECTORY\n"
  "       Use DIRECTORY as the place to find tests to run.  If not set,\n"
  "       use the directory named by the VDOTEST_DIR environment variable.\n"
  "       If neither the --test-directory option nor the VDOTEST_DIR\n"
  "       environment variable is specified, use the current directory.\n"
  "\n"
  "    --timeout=SECONDS\n"
  "       Fail any test which runs for more than SECONDS seconds.\n"
  "\n";

static bool doFork = true;
static bool printElapsedTimes = false;

static int          testArgc = 0;
static uint32_t     timeout  = 0;
static const char **testArgv = NULL;

/**
 * Set the name of the current thread for process listings.
 *
 * The Linux prctl(PR_SET_NAME) call accepts names up to 16 chars
 * long, so don't be too verbose.
 *
 * @param name   name to set for the current thread
 **/
static void setThreadName(const char *name)
{
  /*
   * The name is just advisory for humans examining it, so we don't
   * care much if this fails.
   */
  UDS_ASSERT_SUCCESS(prctl(PR_SET_NAME, (unsigned long) name, 0, 0, 0));
}

/**
 * Set up test index files.
 **/
static void setupFiles(void)
{
  int fd;
  const char *path = getTestIndexName();
  int result = open_file(path, FU_CREATE_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    char errbuf[VDO_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to initialize index file: %s: %s", path,
         uds_string_error(result, errbuf, sizeof(errbuf)));
  }

  close_file(fd, NULL);
}

/**
 * Cleanup any leftover test files
 **/
static void cleanupFiles(void)
{
  const char *path = getTestIndexName();
  int result = remove_file(path);
  if (result != UDS_SUCCESS) {
    char errbuf[VDO_MAX_ERROR_MESSAGE_SIZE];
    warnx("Error removing index file %s: %s", path,
          uds_string_error(result, errbuf, sizeof(errbuf)));
  }
}

/**********************************************************************/
static int parseDirectory(char *arg, const char **testDir)
{
  if (arg != NULL) {
    bool isDir = false;
    int result = is_directory(arg, &isDir);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (isDir) {
      *testDir = arg;
      return UDS_SUCCESS;
    }
  }
  return UDS_NO_DIRECTORY;
}

/**********************************************************************/
static int parseLong(const char *argptr, long *parsed)
{
  char *endptr;
  long arg = 0;

  assert(argptr != NULL);

  errno = 0;
  arg = strtol(argptr, &endptr, 0);

  if (errno == ERANGE || errno == EINVAL ||
      endptr == argptr || *endptr != '\0') {
    return -1;
  } else {
    *parsed = arg;
    return 0;
  }
}

/**********************************************************************/
static int parseInt(const char *argptr, int *parsed)
{
  long arg = 0;

  int result = parseLong(argptr, &arg);

  if (result != 0) {
    return result;
  }

  if (arg < INT_MIN || arg > INT_MAX) {
    return -1;
  } else {
    *parsed = (int) arg;
    return 0;
  }
}

/**********************************************************************/
static int parseUnsignedInt(const char *argptr, unsigned int *parsed)
{
  char *endptr;
  unsigned long arg = 0;

  assert(argptr != NULL);

  /*
   * There are cases where a leading '-' isn't detected below (e.g.,
   * "--foo=-18446744069414588416").  They're only "valid" if you look
   * at the 32-bit unsigned bounds applied to the mod-2^64
   * interpretation of the value, and probably should just be
   * rejected.
   */
  while (isspace(*argptr)) {
    argptr++;
  }
  if (*argptr == '-') {
    return -1;
  }

  errno = 0;
  arg = strtoul(argptr, &endptr, 0);

  if (errno == ERANGE || errno == EINVAL ||
      endptr == argptr || *endptr != '\0' || arg > UINT_MAX) {
    return -1;
  } else {
    *parsed = (unsigned int) arg;
    return 0;
  }
}

/**********************************************************************/
static const char *testDirInitializer(void *handle, void **params, int *ptype)
{
  dlerror();

  void *pars = NULL;
  int   type = 0;
  void *sym = dlsym(handle, "initializeTestDirectory");
  if (sym == NULL) {
    return "test directory initialization failed";
  }

  module_init_function_ptr_t init
    = (module_init_function_ptr_t) (intptr_t) sym;

  pars = (*init)();
  type = TEST_DIRECTORY_INIT;

  if (params) {
    *params = pars;
  }
  if (ptype) {
    *ptype = type;
  }
  return NULL;
}

/**********************************************************************/
static void loadTestDirectoryInitialization(struct module  **module,
                                            CU_TestDirInfo **testDirInfo)
{
  char *moduleName;

  int result = vdo_alloc_sprintf(__func__, &moduleName, "%s/%s",
                                 getTestDirectory(), "__vdotest__init.so");
  if (result != VDO_SUCCESS) {
    errx(1, "failed to allocate test dir init");
  }

  result = load_module(moduleName, testDirInitializer, module);
  if (result != UDS_SUCCESS) {
    free(moduleName);
    *module = NULL;
    *testDirInfo = NULL;
    return;
  }
  free(moduleName);

  *testDirInfo = (CU_TestDirInfo *) (*module)->params;
}

/**********************************************************************/
static const char *testModuleMetaInitializer(void  *handle,
                                             void **params,
                                             int   *ptype)
{
  dlerror();
  void *sym = dlsym(handle, "initializeMultiSuiteModule");
  void *pars = NULL;
  int   type;

  if (sym != NULL) {
    module_init_function_ptr_t init
      = (module_init_function_ptr_t) (intptr_t) sym;

    pars = (*init)();
    type = MULTI_SUITE_MODULE;
  }

  if (pars == NULL) {
    dlerror();
    sym = dlsym(handle, "initializeModule");

    if (sym == NULL) {
      const char *errmsg = dlerror();

      if (errmsg) {
        return errmsg;
      } else {
        return "no initialization function found";
      }
    }

    module_init_function_ptr_t init
      = (module_init_function_ptr_t) (intptr_t) sym;
    pars = (*init)();
    type = SINGLE_SUITE_MODULE;

    if (pars == NULL) {
      return "module initialization failed";
    }
  }

  if (params) {
    *params = pars;
  }
  if (ptype) {
    *ptype = type;
  }
  return NULL;
}

/**********************************************************************/
static void loadTestModules(const char     *pattern,
                            size_t         *moduleCount,
                            struct module **modules)
{
  int result = load_generic_modules(getTestDirectory(),
                                    pattern,
                                    testModuleMetaInitializer,
                                    moduleCount,
                                    modules);
  if (result != UDS_SUCCESS) {
    char errbuf[VDO_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to load modules: %s",
         uds_string_error(result, errbuf, sizeof(errbuf)));
  }
}

static void testChild(CU_TestDirInfo *testDirInfo,
                      CU_SuiteInfo   *suite,
                      CU_TestInfo    *test)
  __attribute__ ((__noreturn__));

/**********************************************************************/
static void runTestDirInit(CU_TestDirInfo *testDirInfo,
                           const char     *suiteName,
                           const char     *name)
{
  if (testDirInfo == NULL) {
    return;
  }
  if (testDirInfo->initializerWithArguments != NULL) {
    vdo_log_info("TESTDIR_INIT: %s:%s", suiteName, name);
    testDirInfo->initializerWithArguments(testArgc, testArgv);
  } else if (testDirInfo->initializer != NULL) {
    vdo_log_info("TESTDIR_INIT: %s:%s", suiteName, name);
    testDirInfo->initializer();
  }
}

/**********************************************************************/
static void runTestDirCleanup(CU_TestDirInfo *testDirInfo,
                              const char     *suiteName,
                              const char     *name)
{
  if ((testDirInfo != NULL) && (testDirInfo->cleaner != NULL)) {
    vdo_log_info("TESTDIR_CLEANUP: %s:%s", suiteName, name);
    testDirInfo->cleaner();
  }
}

/**********************************************************************/
static void alarm_handler(int signum __attribute__((unused)))
{
  vdo_dump_all(vdo, "timeout");
  CU_FAIL("Timedout");
}

/**********************************************************************/
static int testSub(CU_TestDirInfo *testDirInfo,
                   CU_SuiteInfo   *suite,
                   CU_TestInfo    *test)
{
  sigset_t emptySet, savedSet;
  struct sigaction oldaction;
  UDS_ASSERT_SYSTEM_CALL(sigemptyset(&emptySet));
  UDS_ASSERT_SYSTEM_CALL(pthread_sigmask(SIG_BLOCK, &emptySet, &savedSet));
  if (timeout > 0) {
    struct sigaction action;
    action.sa_handler = alarm_handler;
    action.sa_mask    = emptySet;
    action.sa_flags   = 0;
    sigaction(SIGALRM, &action, &oldaction);
  }

  runTestDirInit(testDirInfo, suite->name, test->name);
  alarm(timeout);
  if (suite->initializerWithArguments != NULL) {
    vdo_log_info("SETUP: %s", test->name);
    suite->initializerWithArguments(testArgc, testArgv);
  } else if (suite->initializer != NULL) {
    vdo_log_info("SETUP: %s", test->name);
    suite->initializer();
  }
  alarm(0);

  vdo_log_info("STARTING: %s", test->name);
  alarm(timeout);
  test->func();
  alarm(0);
  vdo_log_info("FINISHED: %s", test->name);

  if (suite->cleaner != NULL) {
    vdo_log_info("CLEANUP: %s", test->name);
    alarm(timeout);
    suite->cleaner();
    alarm(0);
  }

  alarm(timeout);
  runTestDirCleanup(testDirInfo, suite->name, test->name);
  alarm(0);
  killChildren();
  if (timeout > 0) {
    sigaction(SIGALRM, &oldaction, NULL);
  }
  UDS_ASSERT_SYSTEM_CALL(sigprocmask(SIG_SETMASK, &savedSet, NULL));
  return 0;
}

/**********************************************************************/
static void testChild(CU_TestDirInfo *testDirInfo,
                      CU_SuiteInfo   *suite,
                      CU_TestInfo    *test)
{
  exit(testSub(testDirInfo, suite, test));
}

/**********************************************************************/
static TestResult runTest(CU_TestDirInfo *testDirInfo,
                          CU_SuiteInfo   *suite,
                          CU_TestInfo    *test)
{
  TestResult result = {
    .name     = test->name,
    .tests    = 1,
    .errors   = 0,
    .failures = 0,
    .elapsed  = 0.0,
    .numSub   = 0,
    .sub      = NULL,
  };

  if (printElapsedTimes) {
    fprintf(stderr, "  %-50s ", test->name);
  } else {
    fprintf(stderr, "  %s ", test->name);
  }
  fflush(stderr);
  struct timeval tv0;
  gettimeofday(&tv0, NULL);
  if (doFork) {
    pid_t pid = fork();
    if (pid == 0) {
      testChild(testDirInfo, suite, test);
    } else {
      int status;
      int tmp = waitpid(pid, &status, 0);
      if (tmp == -1) {
        err(1, "waitpid");
      }
      assert (tmp == pid);
      if (WIFSIGNALED(status)) {
        if (WCOREDUMP(status)) {
          fprintf(stderr, "(%s Signal, core dumped to core.%d) ",
                  strsignal(WTERMSIG(status)), pid);
        } else {
          fprintf(stderr, "(%s Signal) ", strsignal(WTERMSIG(status)));
        }
        result.failures = 1;
      } else if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        result.failures = 1;
      }
    }
  } else {
    result.failures = testSub(NULL, suite, test);
  }

  struct timeval tv1;
  gettimeofday(&tv1, NULL);
  result.elapsed = (tv2usec(tv1) - tv2usec(tv0)) / 1000000.0;

  if (result.failures > 0) {
    fprintf(stderr, "FAILED\n");
  } else if (!printElapsedTimes) {
    fprintf(stderr, "passed\n");
  } else {
    fprintf(stderr, "passed  %.3f seconds\n", result.elapsed);
  }
  return result;
}

/**********************************************************************/
static void addTestResult(TestResult   *target,
                          unsigned int  index,
                          TestResult    sub)
{
  target->tests      += sub.tests;
  target->errors     += sub.errors;
  target->failures   += sub.failures;
  target->elapsed    += sub.elapsed;
  ++target->numSub;
  target->sub[index]  = sub;
}

/**********************************************************************/
static void freeTestResults(TestResult *tr)
{
  if (tr == NULL) {
    return;
  }
  if (tr->sub == NULL) {
    return;
  }
  for (unsigned int i = 0; i < tr->numSub; ++i) {
    freeTestResults(&tr->sub[i]);
  }
  free(tr->sub);
}

/**********************************************************************/
static void printFailuresToStderr(unsigned int indent, TestResult tr)
{
  if (tr.failures > 0) {
    if (tr.numSub > 0) {
      fprintf(stderr, "%*s%s\n", indent, "", tr.name);
      for (unsigned int i = 0; i < tr.numSub; ++i) {
        printFailuresToStderr(indent + 2, tr.sub[i]);
      }
    } else {
      fprintf(stderr, "%*s%s FAILED\n", indent, "", tr.name);
    }
  }
}

/**********************************************************************/
static void printTestcase(FILE         *fp,
                          unsigned int  indent,
                          const char   *suiteName,
                          TestResult    tr)
{
  fprintf(fp,
          "%*s<testcase classname=\"%s\""
          " name=\"%s\" time=\"%f\"",
          indent, "",
          suiteName,
          tr.name,
          tr.elapsed);
  if (tr.failures > 0) {
    fprintf(fp,
            ">\n%*s<failure message=\"\" type=\"\"/>\n"
            "%*s</testcase>\n>",
            indent + 2, "",
            indent, "");
  } else {
    fprintf(fp, "/>\n");
  }
}

/**********************************************************************/
static void printTestsuite(FILE *fp, unsigned int indent, TestResult tr)
{
  fprintf(fp,
          "%*s<testsuite errors=\"%u\" failures=\"%u\" tests=\"%u\""
          " name=\"%s\""
          " time=\"%f\">\n",
          indent, "",
          tr.errors,
          tr.failures,
          tr.tests,
          tr.name,
          tr.elapsed);
  for (unsigned int i = 0; i < tr.numSub; ++i) {
    printTestcase(fp, indent + 2, tr.name, tr.sub[i]);
  }
  fprintf(fp, "%*s</testsuite>\n",
          indent, "");
}

/**********************************************************************/
static void printTestsuites(FILE *fp, unsigned int indent, TestResult tr)
{
  fprintf(fp,
          "%*s<testsuites errors=\"%u\" failures=\"%u\" tests=\"%u\""
          " name=\"%s\""
          " time=\"%f\">\n",
          indent, "",
          tr.errors,
          tr.failures,
          tr.tests,
          tr.name,
          tr.elapsed);
  for (unsigned int i = 0; i < tr.numSub; ++i) {
    printTestsuite(fp, indent + 2, tr.sub[i]);
  }
  fprintf(fp, "%*s</testsuites>\n",
          indent, "");
}

/**********************************************************************/
static void printXmlResults(const char *filename, TestResult tr)
{
  FILE *fp = fopen(filename, "w");
  fprintf(fp, "<?xml version=\"1.0\"?>\n");
  printTestsuites(fp, 0, tr);
  fclose(fp);
}

/**********************************************************************/
static TestResult runSuite(CU_TestDirInfo *testDirInfo, CU_SuiteInfo *suite)
{
  fprintf(stderr, "Running suite %s\n", suite->name);
  vdo_log_info("STARTING SUITE: %s", suite->name);
  TestResult result = { .name = suite->name };

  unsigned int numTests = 0;
  for (CU_TestInfo *tests = suite->tests; tests->name != NULL; ++tests) {
    ++numTests;
  }
  result.sub = malloc(sizeof(*result.sub) * numTests);

  for (unsigned int i = 0; i < numTests; i++) {
    addTestResult(&result, i, runTest(testDirInfo, suite, &suite->tests[i]));
  }
  vdo_log_info("DONE SUITE: %s", suite->name);
  if (printElapsedTimes) {
    fprintf(stderr, "%10.3f seconds to complete %2d tests in suite %s\n",
            result.elapsed, result.tests, suite->name);
  }
  return result;
}

/**********************************************************************/
static int runSuites(CU_TestDirInfo *testDirInfo,
                     CU_SuiteInfo   *suites,
                     const char     *xml)
{
  TestResult result = { .name = "checkin" };

  unsigned int numSuites = 0;
  for (CU_SuiteInfo *s = suites; s->name != NULL; ++s) {
    ++numSuites;
  }
  result.sub = malloc(sizeof(*result.sub) * numSuites);

  if (!doFork) {
    runTestDirInit(testDirInfo, "(all suites)", getTestDirectory());
  }
  for (unsigned int i = 0; i < numSuites; i++) {
    addTestResult(&result, i, runSuite(testDirInfo, &suites[i]));
  }
  if (!doFork) {
    runTestDirCleanup(testDirInfo, "(all suites)", getTestDirectory());
  }
  int ret = (result.failures == 0) ? 0 : 1;
  if (xml != NULL) {
    printXmlResults(xml, result);
  } else {
    printFailuresToStderr(0, result);
  }
  if (printElapsedTimes) {
    fprintf(stderr, "%10.3f seconds to complete all %d tests\n",
            result.elapsed, result.tests);
  }
  freeTestResults(&result);
  return ret;
}

/**
 * The main() function for setting up and running the tests.
 * Returns zero on success, 1 otherwise.
 **/
int main(int argc, char **argv)
{
  struct module *moduleList[argc];
  int            moduleIndex   = 0;
  size_t         count         = 0;
  int            repCount      = 1;
  char          *xml           = NULL;
  int            ret           = 0;
  int            unload        = true;
  int            c;
  unsigned int   specifiedSeed = 0;
  const char    *testDirectory = NULL;

  struct option options[] = {
    {"help",           no_argument,       NULL, 'h'},
    {"repeat",         optional_argument, NULL, 'r'},
    {"xml",            required_argument, NULL, 'x'},
    {"seed",           required_argument, NULL, 's'},
    {"no-unload",      no_argument,       NULL, 'n'},
    {"no-fork",        no_argument,       NULL, 'f'},
    {"elapsed",        no_argument,       NULL, 'e'},
    {"test-directory", required_argument, NULL, 'd'},
    {"timeout",        required_argument, NULL, 't'},
    {NULL,             0,                 NULL,  0 },
  };

  while ((c = getopt_long(argc, argv, "", options, NULL)) != -1) {
    switch (c) {
    case 'h':
      printf("%s", helpString);
      exit(0);
      break;

    case 'd':
      if (parseDirectory(optarg, &testDirectory) != UDS_SUCCESS) {
        errx(1, "The argument to --test-directory"
             " must be a directory containing tests");
      }
      setTestDirectory(testDirectory);
      break;

    case 'e':
      printElapsedTimes = true;
      break;

    case 'f':
      doFork = false;
      break;

    case 'n':
      unload = false;
      break;

    case 'r':
      if (optarg != NULL) {
        if ((parseInt(optarg, &repCount) != UDS_SUCCESS) || repCount <= 0)
          errx(1, "The argument to --repeat, if present,"
               " must be a positive integer ");
      } else {
        repCount = -1;          /* -1 means repeat indefinitely */
      }
      break;

    case 's':
      if (parseUnsignedInt(optarg, &specifiedSeed) != UDS_SUCCESS) {
        errx(1, "The argument to --seed must be an unsigned integer");
      }

      break;

    case 't':
      if (parseUnsignedInt(optarg, &timeout) != UDS_SUCCESS) {
        errx(1, "The argument to --timeout must be an unsigned integer");
      }

      break;

    case 'x':
      xml = optarg;
      break;

    default:
      fprintf(stderr, "Usage: %s%s\n", argv[0], usageString);
      exit(1);
    }
  }

  umask(0);
  setThreadName("main");

  testArgv = calloc(argc - optind, sizeof(char *));

  struct module  *testDirModule = NULL;
  CU_TestDirInfo *testDirInfo   = NULL;

  loadTestDirectoryInitialization(&testDirModule, &testDirInfo);

  // load all the tests and copy their suite information
  if (optind < argc) {
    /*
     * Caller has specified command line argument to vdotest. This could mean
     * one of three things:
     * - run all the tests starting with the specified name
     * - run the specified tests
     * - pass test-specific switches to the test's set-up function
     */

    for (; optind < argc; optind++) {
      const char *arg = argv[optind];

      // Push switches onto a list to pass to the test's set-up function.
      if (arg[0] == '-') {
        testArgv[testArgc++] = arg;
        continue;
      }

      // Try for the specifically named test
      loadTestModules(arg, &count, &moduleList[moduleIndex]);
      if (count == 0) {
        char *pattern;

        // Check for tests using the argument as wildcard
        if (asprintf(&pattern, "%s_t[0-9]*", arg) == -1) {
          err(1, "Failed to allocate pattern buffer");
        }
        loadTestModules(pattern, &count, &moduleList[moduleIndex]);
        free(pattern);
      }
      if (count == 0) {
        errx(1, "%s doesn't match any tests", arg);
      }
      moduleIndex++;
    }
  } else {
    // Run all the tests.
    loadTestModules("*_t[0-9]*", &count, &moduleList[moduleIndex++]);
  }

  size_t suiteCount = 0;

  for (int i = 0; i < moduleIndex; i++) {
    for (struct module *m = moduleList[i]; m != NULL; m = m->next) {
      switch (m->ptype) {
      case SINGLE_SUITE_MODULE:
        ++suiteCount;
        break;
      case MULTI_SUITE_MODULE:
        {
          CU_SuiteInfo **suites = (CU_SuiteInfo **) m->params;
          while (*suites != NULL) {
            ++suiteCount;
            ++suites;
          }
        }
      break;
      default:
        errx(1, "unknown module type %d", m->ptype);
        break;
      }
    }
  }

  if (suiteCount > 0) {
    CU_SuiteInfo *suites = malloc((suiteCount + 1) * sizeof(CU_SuiteInfo));

    CU_SuiteInfo *s = suites;
    for (int i = 0; i < moduleIndex; i++) {
      for (struct module *m = moduleList[i]; m != NULL; m = m->next) {
        switch (m->ptype) {
        case SINGLE_SUITE_MODULE:
          {
            CU_SuiteInfo *info = (CU_SuiteInfo *) m->params;
            *s++ = *info;
          }
          break;
        case MULTI_SUITE_MODULE:
          {
            CU_SuiteInfo **suites = (CU_SuiteInfo **) m->params;
            while (*suites != NULL) {
              *s++ = **suites++;
            }
          }
          break;
        default:
          errx(1, "unknown module type %d", m->ptype);
          break;
        }
      }
    }

    // terminate the suite list
    *s = nullSuite;

    cleanupFiles();
    setupFiles();

    // run the test the specified number of times (or until failure)
    for (int iteration = 0; iteration != repCount; iteration++) {
      if (repCount != 1) {
        printf("\niteration %d/", iteration + 1);
        if (repCount == -1) {
          printf("FOREVER\n");
        } else {
          printf("%d\n", repCount);
        }
      }

      // Seed the random number generator for tests that require it.
      unsigned int seed;
      if (specifiedSeed == 0) {
        seed = (unsigned int) time(NULL);
      } else {
        seed = specifiedSeed;
      }
      vdo_log_info("Using random seed %u", seed);
      srandom(seed);

      ret = runSuites(testDirInfo, suites, xml);
      if (ret != 0) {
        break;
      }
    }

    for (int i = 0; i < moduleIndex; i++) {
      unload_modules(moduleList[i], unload);
    }
    if (testDirModule) {
      unload_modules(testDirModule, unload);
    }
    free(suites);
    cleanupFiles();
  } else {
    ret = 1;
  }

  free(testArgv);

  return ret;
}
