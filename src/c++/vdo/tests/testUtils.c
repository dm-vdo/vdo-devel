/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testUtils.h"

#include <stdlib.h>

static const char *testDirectory = NULL;

/**********************************************************************/
const char *getTestDirectory(void)
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
void setTestDirectory(const char *directory)
{
  testDirectory = directory;
}
