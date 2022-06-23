// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "random.h"
#include "uds.h"

static void testNULL(void)
{
  CU_ASSERT_TRUE(uds_string_error(0, NULL, 0) == NULL);
}

static void testLength(void)
{
  char buf[256];
  unsigned int i, n;
  for (n = 0; n < sizeof(buf); ++n) {
    int c = random() % 256;
    memset(buf, c, sizeof(buf));
    const char *ret = uds_string_error(0, buf, n);
    CU_ASSERT_PTR_EQUAL(ret, buf);
    if (n > 0) {
      CU_ASSERT_TRUE(strlen(buf) < n);
    }
    for (i = n; i < sizeof(buf); ++i) {
      CU_ASSERT_EQUAL((unsigned char)buf[i], c);
    }
  }
}

static const CU_TestInfo tests[] = {
  { "null",   testNULL   },
  { "length", testLength   },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "StringError_t1",
  .tests = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
