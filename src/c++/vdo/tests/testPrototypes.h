/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef TEST_PROTOTYPES_H
#define TEST_PROTOTYPES_H

static const char *testIndexName = NULL;

/**
 * Get the test index name.
 *
 * @return The test index name
 **/
__attribute__ ((__warn_unused_result__))
static inline const char *getTestIndexName(void)
{
  if (testIndexName == NULL) {
    testIndexName = getenv("VDO_TESTINDEX");
    if (testIndexName == NULL) {
      testIndexName = "/u1/zubenelgenubi";
    }
  }

  return testIndexName;
}

#endif /* TEST_PROTOTYPES_H */
