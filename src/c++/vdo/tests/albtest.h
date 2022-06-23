/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef ALBTEST_H
#define ALBTEST_H

typedef struct {
  const char *name;
  void (*func)(void);
} CU_TestInfo;

#define CU_TEST_INFO_NULL { NULL, NULL }
#define CU_SUITE_INFO_NULL { NULL, NULL, NULL, NULL, NULL }

typedef struct {
  const char *name;
  void (*initializerWithArguments)(int argc, const char **argv);
  void (*initializer)(void);
  void (*cleaner)(void);
  CU_TestInfo *tests;
} CU_SuiteInfo;

typedef struct {
  void (*initializerWithArguments)(int argc, const char **argv);
  void (*initializer)(void);
  void (*cleaner)(void);
} CU_TestDirInfo;

/*
 * A test module must define one of these two init routines.
 */
extern CU_SuiteInfo    *initializeModule(void);
extern CU_SuiteInfo   **initializeMultiSuiteModule(void);
extern CU_TestDirInfo  *initializeTestDirectory(void);

#endif /* ALBTEST_H */
