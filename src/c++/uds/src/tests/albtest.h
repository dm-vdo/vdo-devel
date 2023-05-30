/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef ALBTEST_H
#define ALBTEST_H

#include "uds.h"

typedef struct {
  const char *name;
  void (*func)(void);
} CU_TestInfo;

#define CU_TEST_INFO_NULL { NULL, NULL }

typedef struct cu_SuiteInfo {
  /* Suite name.  Should include the filename of the test, and should avoid
   * using "[]{}" */
  const char *name;
  /* Initializers.  All of these that are not null are invoked. */
  void (*initializerWithArguments)(int argc, const char **argv);
  void (*initializerWithIndexName)(const char *indexName);
  void (*initializerWithSession)(struct uds_index_session *indexSession);
  void (*initializer)(void);
  /* Cleaner.  Called after the test if it is not null. */
  void (*cleaner)(void);
  /* List of tests */
  const CU_TestInfo *tests;
  /* Link to the next suite */
  const struct cu_SuiteInfo *next;
  /* Name of the index, filled in by expandSuites */
  const char *indexName;
  /* If this flag is set, the suite must be run.  Any testing options that
   * run a subset of the suites must not prevent this suite from being run
   * at least once. */
  bool mustRun;
  /* If this flag is set, no sparse index sessions are created for the
   * test.  Only applies when there is an initializerWithSession.  */
  bool noSparse;
  /* If this flag is set, only one index name is used, and the default
   * index size is 1GB, and argc/argv can modify the index configuration.
   * Only applies when there is an initializerWithSession.  */
  bool oneIndexConfiguredByArgv;
  /* If this flag is set, a sparse index session is created for the test.
   * Filled in by expandSuites */
  bool useSparseSession;
} CU_SuiteInfo;

extern bool albtestSkipFlag;

static inline void skipThisTest(void)
{
  albtestSkipFlag = true;
}

static inline bool wasTestSkipped(void)
{
  return albtestSkipFlag;
}

// A test module must define this init routine.
extern const CU_SuiteInfo  *initializeModule(void);

////////////////////////////////////////////////////////////////////////
//// A test module can call these routines ////
////////////////////////////////////////////////////////////////////////

/**
 * Flush output buffers, as like to fflush as possible
 **/
void albFlush(void);

/**
 * Output a message, as like to printf as possible
 **/
void albPrint(const char *format, ...) __printf(1, 2);

#endif /* ALBTEST_H */
