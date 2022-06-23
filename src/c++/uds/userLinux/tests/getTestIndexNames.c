/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testPrototypes.h"

#include <stdlib.h>

#include "assertions.h"

static const char *testIndexName = NULL;

/**********************************************************************/
const char * const *getTestIndexNames(void)
{
  if (testIndexName == NULL) {
    testIndexName = getenv("UDS_TESTINDEX");
    if (testIndexName == NULL) {
      testIndexName = "/u1/zubenelgenubi";
    }
  }

  static const char *names[2];
  names[0] = testIndexName;
  names[1] = NULL;

  return names;
}

/**********************************************************************/
const char *const *getTestMultiIndexNames(void)
{
  static const char *const names[3] = {
    "/u1/zubenelgenubi-0",
    "/u1/zubenelgenubi-1",
    NULL,
  };
  return names;
}
