/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef ALBTESTCOMMON_H
#define ALBTESTCOMMON_H

#include "albtest.h"
#include "time-utils.h"

typedef struct testResult {
  const char        *name;
  ktime_t            elapsed;
  unsigned int       tests;
  unsigned int       errors;
  unsigned int       failures;
  unsigned int       skips;
  unsigned int       numSub;
  struct testResult *sub;
  bool               freeName;
} TestResult;

// we pass these values to suite->initializerWithArguments()
extern int          testArgc;
extern const char **testArgv;

/**
 * Add a test result, propagating values upward
 *
 * @param target  The higher level build target
 * @param index   Index of the result to be recorded
 * @param sub     The test result
 **/
void addTestResult(TestResult *target, unsigned int index, TestResult sub);

/**
 * Free test results
 *
 * @param tr  Test results
 **/
void freeTestResults(TestResult *tr);

/**
 * Copy a single suite
 *
 * @param suite  The suite to copy
 *
 * @return The copy of the suite
 **/
CU_SuiteInfo *copySuite(const CU_SuiteInfo *suite);

/**
 * Append a list of suites to a list of suites.  This method is used to add
 * the suites from a single module to the list of all suites.
 *
 * @param ppSuitesTail  Pointer at the tail end of the list of suites.
 * @param suites        The first suite to add
 **/
void appendSuites(const CU_SuiteInfo ***ppSuitesTail,
                  const CU_SuiteInfo *suites);

/**
 * Run a test.
 *
 * @param suite  The suite containing the test
 * @param test   The test
 *
 * @note If the tests passes, it will return normally.
 *       If the test fails, it will trigger an assertion.
 **/
void testSub(const CU_SuiteInfo *suite, const CU_TestInfo *test);

/**
 * Run a single suite
 *
 * @param suite  The suite
 *
 * @return the test results
 **/
TestResult runSuite(const CU_SuiteInfo *suite)
  __attribute__((warn_unused_result));

/**
 * Run a list of suites
 *
 * @param suites  The first suite
 *
 * @return the test results
 **/
TestResult runSuites(const CU_SuiteInfo *suites)
  __attribute__((warn_unused_result));

/**
 * Free a list of suites
 *
 * @param suites  The first suite
 **/
void freeSuites(const CU_SuiteInfo *suites);

// ========================================================================
// These methods are called from albtestCommon and must be defined by the
// calling module
// ========================================================================

/**
 * Run a single test.  We expect this method to call testSub, but it may do
 * platform dependent things, such as forking a child process to run the
 * test.
 *
 * @param suite  The suite containing the test
 * @param test   The test
 *
 * @return the test results
 **/
TestResult runTest(const CU_SuiteInfo *suite, const CU_TestInfo *test);

/**
 * Print the summary of the test results. (This function is recursive).
 *
 * @param indent        indent for printing
 * @param tr            the test result
 **/
void printSummary(unsigned int indent, TestResult tr);

/**
 * Print a test or suite name.
 *
 * @param indent        indent for printing
 * @param name          the test or suite name
 * @param failures      total number of failures
 * @param skips         total number of skips
 **/
void printName(unsigned int indent, const char *name,
               unsigned int failures, unsigned int skips);

/**
 * Print a test case result.
 *
 * @param indent        the indent for printing
 * @param name          the test case name
 * @param result        the test case result
 **/
void printTestResult(unsigned int indent, const char *name, const char *result);

#endif /* ALBTESTCOMMON_H */
