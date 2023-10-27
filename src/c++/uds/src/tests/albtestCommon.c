// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtestCommon.h"

#include "assertions.h"
#include "dory.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "testPrototypes.h"

int          testArgc;
const char **testArgv;

bool albtestSkipFlag = false;

/**********************************************************************/
void addTestResult(TestResult *target, unsigned int index, TestResult sub)
{
  target->tests      += sub.tests;
  target->errors     += sub.errors;
  target->failures   += sub.failures;
  target->skips      += sub.skips;
  target->elapsed    += sub.elapsed;
  ++target->numSub;
  target->sub[index]  = sub;
}

/**********************************************************************/
void freeTestResults(TestResult *tr)
{
  if ((tr != NULL) && (tr->sub != NULL)) {
    unsigned int i;
    for (i = 0; i < tr->numSub; ++i) {
      freeTestResults(&tr->sub[i]);
    }
    uds_free(tr->sub);
    tr->sub = NULL;
    if (tr->freeName) {
      uds_free_const(tr->name);
      tr->name = NULL;
      tr->freeName = false;
    }
  }
}

/**********************************************************************/
CU_SuiteInfo *copySuite(const CU_SuiteInfo *suite)
{
  CU_SuiteInfo *s;
  UDS_ALLOCATE(1, CU_SuiteInfo, "suite", &s);
  *s = *suite;
  s->useSparseSession = false;
  s->next = NULL;
  return s;
}

/**********************************************************************/
static void appendToSuiteList(const CU_SuiteInfo ***ppSuitesTail,
                              CU_SuiteInfo *suite)
{
  **ppSuitesTail = suite;
  *ppSuitesTail  = &suite->next;
}

/**********************************************************************/
void appendSuites(const CU_SuiteInfo ***ppSuitesTail,
                  const CU_SuiteInfo *suites)
{
  const CU_SuiteInfo *suite;
  for (suite = suites; suite != NULL; suite = suite->next) {
    appendToSuiteList(ppSuitesTail, copySuite(suite));
  }
}

/**********************************************************************/
static const CU_SuiteInfo *expandSuites(const CU_SuiteInfo *suites)
{
  const CU_SuiteInfo *suiteList = NULL;
  const CU_SuiteInfo **pSuites = &suiteList;
  const CU_SuiteInfo *suite;
  for (suite = suites; suite != NULL; suite = suite->next) {
    if ((suite->initializerWithBlockDevice != NULL)
        || (suite->initializerWithSession != NULL)) {
      // Suite uses a test block device
      CU_SuiteInfo *s = copySuite(suite);
      s->bdev = getTestBlockDevice();
      appendToSuiteList(&pSuites, s);
      if (suite->oneIndexConfiguredByArgv) {
        // Suite runs on only one index
        break;
      }
      if (!suite->noSparse && (suite->initializerWithSession != NULL)) {
        s = copySuite(suite);
        s->bdev = getTestBlockDevice();
        s->useSparseSession = true;
        appendToSuiteList(&pSuites, s);
      }
    } else {
      appendToSuiteList(&pSuites, copySuite(suite));
    }
  }
  return suiteList;
}

/**********************************************************************/
static struct uds_index_session *testOpenIndex(const CU_SuiteInfo *suite)
{
  struct uds_index_session *session;
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .sparse = suite->useSparseSession,
  };
  if (suite->oneIndexConfiguredByArgv) {
    params = createUdsParametersForAlbtest(testArgc, testArgv);
  }
  params.bdev = suite->bdev;
  randomizeUdsNonce(&params);
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  return session;
}

/**********************************************************************/
void testSub(const CU_SuiteInfo *suite, const CU_TestInfo *test)
{
  struct uds_index_session *indexSession = NULL;
  albtestSkipFlag = false;
  if (suite->initializerWithArguments != NULL) {
    suite->initializerWithArguments(testArgc, testArgv);
  }
  if (suite->initializerWithBlockDevice != NULL) {
    suite->initializerWithBlockDevice(suite->bdev);
  }
  if (suite->initializerWithSession != NULL) {
    indexSession = testOpenIndex(suite);
    suite->initializerWithSession(indexSession);
  }
  // The suite can use multiple initializers.  We agree to always call the
  // one without arguments last.
  if (suite->initializer != NULL) {
    suite->initializer();
  }
  test->func();
  if (suite->cleaner != NULL) {
    suite->cleaner();
  }
  if (suite->initializerWithSession != NULL) {
    UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
    UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  }
}

/**********************************************************************/
TestResult runSuite(const CU_SuiteInfo *suite)
{
  TestResult result = { .name = suite->name };
  if (suite->bdev != NULL) {
    const char *sparseSuffix = suite->useSparseSession ? " {sparse}" : "";
    char *name;
    UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &name, "%s %s",
                                         suite->name, sparseSuffix));
    result.name     = name;
    result.freeName = true;
  }

  unsigned int numTests = 0;
  const CU_TestInfo *tests;
  for (tests = suite->tests; tests->name != NULL; ++tests) {
    ++numTests;
  }
  UDS_ALLOCATE(numTests, TestResult, "Test Results", &result.sub);

  albPrint("Running suite %s", result.name);
  unsigned int i;
  for (i = 0; i < numTests; i++) {
    set_dory_forgetful(false);
    addTestResult(&result, i, runTest(suite, &suite->tests[i]));
  }
  return result;
}

/**********************************************************************/
TestResult runSuites(const CU_SuiteInfo *suites)
{
  TestResult result = { .name = "Results" };
  const CU_SuiteInfo *expanded = expandSuites(suites);

  unsigned int numSuites = 0;
  const CU_SuiteInfo *s;
  for (s = expanded; s != NULL; s = s->next) {
    ++numSuites;
  }
  UDS_ALLOCATE(numSuites, TestResult, "Suite Results", &result.sub);

  unsigned int index = 0;
  for (s = expanded; s != NULL; s = s->next) {
    addTestResult(&result, index++, runSuite(s));
  }

  freeSuites(expanded);
  return result;
}

/**********************************************************************/
void freeSuites(const CU_SuiteInfo *suites)
{
  while (suites != NULL) {
    const CU_SuiteInfo *s = suites;
    suites = s->next;
    putTestBlockDevice(s->bdev);
    uds_free_const(s);
  }
}

/**********************************************************************/
void printSummary(unsigned int indent, TestResult tr)
{
  if (tr.failures + tr.skips > 0) {
    if (tr.numSub > 0) {
      printName(indent, tr.name, tr.failures, tr.skips);
      unsigned int i;
      for (i = 0; i < tr.numSub; ++i) {
        printSummary(indent + 2, tr.sub[i]);
      }
    } else if (tr.failures) {
      printTestResult(indent, tr.name, "FAILED");
    } else if (tr.skips) {
      printTestResult(indent, tr.name, "(skipped)");
    }
  }
}

/**********************************************************************/
void printName(unsigned int  indent,
               const char   *name,
               unsigned int  failures,
               unsigned int  skips)
{
  albPrint("%*s%s (%u failed, %u skipped)", indent, "", name, failures, skips);
}

/**********************************************************************/
void printTestResult(unsigned int indent, const char *name, const char *result)
{
  unsigned int len = strlen(name);
  unsigned int col = 60;

  if (len + indent + 2 > col) {
    albPrint("%*s%s\n%*s%s", indent, "", name, col, "", result);
  } else {
    albPrint("%*s%s%*s%s", indent, "", name, col - len - indent, "", result);
  }
}
