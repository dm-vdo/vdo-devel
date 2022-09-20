/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "albtest.h"
#include "albtestCommon.h"
#include "assertions.h"
#include "directoryUtils.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "modloader.h"
#include "testPrototypes.h"

enum {
  SINGLE_SUITE_MODULE = 1,
};

static const char usageString[] =
  " [--help]"
  " [--xml=FILENAME] [--repeat[=N]] [--no-unload] [--seed=SEED]"
  " [--no-fork] [--elapsed] [--test-directory=DIRECTORY]"
  " [pattern ...] [-- <test-specific options>]";

static const char helpString[] =
  "albtest - run unit tests\n"
  "\n"
  "SYNOPSIS\n"
  "  albtest [options] [pattern ...] [-- testoptions]\n"
  "\n"
  "DESCRIPTION\n"
  "  albtest runs the test or tests that match [pattern ...] which is\n"
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
  "       use the directory named by the ALBTEST_DIR environment variable.\n"
  "       If neither the --test-directory option nor the ALBTEST_DIR\n"
  "       environment variable is specified, use the current directory.\n"
  "\n";

static bool doFork = true;

static const char *testDirectory = NULL;

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
static const char *testDir(void)
{
  if (testDirectory == NULL) {
    testDirectory = getenv("ALBTEST_DIR");
    if (testDirectory == NULL) {
      testDirectory = ".";
    }
  }
  return testDirectory;
}

/**********************************************************************/
static int parseDirectory(const char *arg, const char **testDir)
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
static int parseRepetitions(const char *arg, int *repeats)
{
  char *endPtr;
  long value;

  errno = 0;
  value = strtol(arg, &endPtr, 0);
  if ((errno == ERANGE) || (errno == EINVAL) || (*endPtr != '\0')
      || (value <= 0) || (value > INT_MAX)) {
    return UDS_OUT_OF_RANGE;
  }

  *repeats = value;
  return UDS_SUCCESS;
}

/**********************************************************************/
static int parseSeed(const char *arg, unsigned int *seed)
{
  char *endPtr;
  unsigned long value;

  errno = 0;
  value = strtoul(arg, &endPtr, 0);
  if ((errno == ERANGE) || (errno == EINVAL) || (*endPtr != '\0')
      || (value > UINT_MAX)) {
    return UDS_OUT_OF_RANGE;
  }

  *seed = value;
  return UDS_SUCCESS;
}

/**********************************************************************/
static void createTestFile(const char *path)
{
  int fd;
  int result;

  result = open_file(path, FU_CREATE_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to initialize test files: %s: %s",
         uds_string_error(result, errbuf, sizeof(errbuf)),
         path);
  }
  close_file(fd, NULL);
}

/**********************************************************************/
static void setupTestState(void)
{
  const char *const *names = getTestIndexNames();
  while (*names != NULL) {
    createTestFile(*names);
    names++;
  }

  names = getTestMultiIndexNames();
  while (*names != NULL) {
    createTestFile(*names);
    names++;
  }
}

/**********************************************************************/
static const char *testModuleMetaInitializer(void  *handle,
                                             void **params,
                                             int   *ptype)
{
  dlerror();
  void *sym = dlsym(handle, "initializeModule");
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
  void *pars = (*init)();
  int type = SINGLE_SUITE_MODULE;
  if (pars == NULL) {
    return "module initialization failed";
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
  int result = load_generic_modules(testDir(),
                                    pattern,
                                    testModuleMetaInitializer,
                                    moduleCount,
                                    modules);
  if (result != UDS_SUCCESS) {
    char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];
    errx(1, "Failed to load modules: %s",
         uds_string_error(result, errbuf, sizeof(errbuf)));
  }
}

/**********************************************************************/
static void testChild(const CU_SuiteInfo *suite, const CU_TestInfo *test)
{
  testSub(suite, test);
  killChildren();
  if (wasTestSkipped()) {
    exit(99);
  }
  exit(0);
}

/**********************************************************************/
TestResult runTest(const CU_SuiteInfo *suite, const CU_TestInfo *test)
{
  TestResult result = {
    .name  = test->name,
    .tests = 1,
  };

  printf("  %s...\n", test->name);
  fflush(NULL);
  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  if (doFork) {
    pid_t pid = fork();
    if (pid == 0) {
      testChild(suite, test);
    } else {
      int status;
      int tmp = waitpid(pid, &status, 0);
      if (tmp == -1) {
        err(1, "waitpid");
      }
      assert (tmp == pid);
      if (WIFSIGNALED(status)) {
        if (WCOREDUMP(status)) {
          printf("(%s Signal, core dumped to core.%d) ",
                  strsignal(WTERMSIG(status)), pid);
        } else {
          printf("(%s Signal) ", strsignal(WTERMSIG(status)));
        }
        result.failures = 1;
      } else if (WIFEXITED(status) && (WEXITSTATUS(status) == 99)) {
        result.skips = 1;
      } else if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        result.failures = 1;
      }
    }
  } else {
    sigset_t emptySet, savedSet;
    UDS_ASSERT_SYSTEM_CALL(sigemptyset(&emptySet));
    CU_ASSERT_EQUAL(0, pthread_sigmask(SIG_BLOCK, &emptySet, &savedSet));
    testSub(suite, test);
    CU_ASSERT_EQUAL(0, pthread_sigmask(SIG_SETMASK, &savedSet, NULL));
    killChildren();
    result.failures = 0;
    result.skips = wasTestSkipped();
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
static void printElapsedTimes(unsigned int indent, TestResult tr)
{
  char *elapsed;
  if (rel_time_to_string(&elapsed, tr.elapsed) == UDS_SUCCESS) {
    printf("%*s%s %-20s\n", indent, "", tr.name, elapsed);
    free(elapsed);
  }
  for (unsigned int i = 0; i < tr.numSub; ++i) {
    printElapsedTimes(indent + 2, tr.sub[i]);
  }
}

/**********************************************************************/
static void printTestElapsed(FILE *fp, ktime_t elapsed)
{
  char *elapsedTime;
  if ((elapsed > 0)
      && (rel_time_to_string(&elapsedTime, elapsed) == UDS_SUCCESS)) {
    fprintf(fp, " time=\"%s\"", elapsedTime);
    UDS_FREE(elapsedTime);
  }
}

/**********************************************************************/
static void printTestcase(FILE         *fp,
                          unsigned int  indent,
                          const char   *suiteName,
                          TestResult    tr)
{
  fprintf(fp, "%*s<testcase classname=\"%s\" name=\"%s\"",
          indent, "", suiteName, tr.name);
  printTestElapsed(fp, tr.elapsed);
  if (tr.failures > 0) {
    fprintf(fp, ">\n%*s<failure message=\"\" type=\"\"/>\n%*s</testcase>\n",
            indent + 2, "", indent, "");
  } else if (tr.skips > 0) {
    fprintf(fp, ">\n%*s<skipped count=\"%u\"/>\n%*s</testcase>\n",
            indent + 2, "", tr.skips, indent, "");
  } else {
    fprintf(fp, "/>\n");
  }
}

/**********************************************************************/
static void printTestsuite(FILE *fp, unsigned int indent, TestResult tr)
{
  fprintf(fp,
          "%*s<testsuite errors=\"%u\" failures=\"%u\" skips=\"%u\" "
          "tests=\"%u\" name=\"%s\"",
          indent, "", tr.errors, tr.failures, tr.skips, tr.tests, tr.name);
  printTestElapsed(fp, tr.elapsed);
  fprintf(fp, ">\n");
  for (unsigned int i = 0; i < tr.numSub; ++i) {
    printTestcase(fp, indent + 2, tr.name, tr.sub[i]);
  }
  fprintf(fp, "%*s</testsuite>\n", indent, "");
}

/**********************************************************************/
static void printTestsuites(FILE *fp, unsigned int indent, TestResult tr)
{
  fprintf(fp,
          "%*s<testsuites errors=\"%u\" failures=\"%u\" skips=\"%u\" "
          "tests=\"%u\" name=\"%s\"",
          indent, "", tr.errors, tr.failures, tr.skips, tr.tests, tr.name);
  printTestElapsed(fp, tr.elapsed);
  fprintf(fp, ">\n");
  for (unsigned int i = 0; i < tr.numSub; ++i) {
    printTestsuite(fp, indent + 2, tr.sub[i]);
  }
  fprintf(fp, "%*s</testsuites>\n", indent, "");
}

/**********************************************************************/
static void printXmlResults(const char *filename, TestResult tr)
{
  FILE *fp = fopen(filename, "w");
  fprintf(fp, "<?xml version=\"1.0\"?>\n");
  printTestsuites(fp, 0, tr);
  fclose(fp);
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
  bool           elapsedTimes  = false;

  struct option options[] = {
    {"help",           no_argument,       NULL, 'h'},
    {"repeat",         optional_argument, NULL, 'r'},
    {"xml",            required_argument, NULL, 'x'},
    {"seed",           required_argument, NULL, 's'},
    {"no-unload",      no_argument,       NULL, 'n'},
    {"no-fork",        no_argument,       NULL, 'f'},
    {"elapsed",        no_argument,       NULL, 'e'},
    {"test-directory", required_argument, NULL, 'd'},
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
      break;
    case 'e':
      elapsedTimes = true;
      break;
    case 'f':
      doFork = false;
      break;
    case 'n':
      unload = false;
      break;
    case 'r':
      if (optarg != NULL) {
        if (parseRepetitions(optarg, &repCount) != UDS_SUCCESS) {
          errx(1, "The argument to --repeat, if present,"
               " must be a positive integer ");
        }
      } else {
        repCount = -1;          /* -1 means repeat indefinitely */
      }
      break;
    case 's':
      if (parseSeed(optarg, &specifiedSeed) != UDS_SUCCESS) {
        errx(1, "The argument to --seed must be an unsigned integer");
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
  open_uds_logger();

  testArgv = calloc(argc - optind, sizeof(char *));

  // load all the tests and copy their suite information
  if (optind < argc) {
    /*
     * Caller has specified command line argument to albtest. This could mean
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

  setupTestState();

  const CU_SuiteInfo *suites = NULL;
  const CU_SuiteInfo **pSuites = &suites;

  for (int i = 0; i < moduleIndex; i++) {
    for (struct module *m = moduleList[i]; m != NULL; m = m->next) {
      switch (m->ptype) {
      case SINGLE_SUITE_MODULE:
        appendSuites(&pSuites, m->params);
        break;
      default:
        errx(1, "unknown module type %d", m->ptype);
        break;
      }
    }
  }

  if (suites != NULL) {
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
      srandom(seed);

      TestResult result = runSuites(suites);

      if (xml != NULL) {
        printXmlResults(xml, result);
      } else {
        printSummary(0, result);
      }

      if (elapsedTimes) {
        printElapsedTimes(0, result);
      }

      ret = (result.failures == 0) ? 0 : 1;
      freeTestResults(&result);
      if (ret != 0) {
        break;
      }
    }

    for (int i = 0; i < moduleIndex; i++) {
      unload_modules(moduleList[i], unload);
    }
    freeSuites(suites);
  } else {
    ret = 1;
  }

  free(testArgv);

  return ret;
}
