/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/cache.h>
#include <stdio.h>

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "random.h"
#include "string-utils.h"
#include "uds.h"

/*
 * If we're compiled without _GNU_SOURCE, we get a version of
 * strerror_r that returns an int. Since these calls appear in varargs
 * lists, they're candidates for not being caught at compile time. See
 * strerror(3) for details.
 */
static void badStrerrorReturn(void)
{
  char buf[128];

  char *result = strerror_r(ENOMEM, buf, sizeof(buf));
  CU_ASSERT_STRING_EQUAL(result, "Cannot allocate memory");
}

/**********************************************************************/
static void testAllocateCacheAligned(void)
{
  // No real reason to try test posix_memalign(), but we need to make some
  // effort to verify that we're calling it correctly.
  enum {
    ITERATIONS = 100,
    LINE_MASK = L1_CACHE_BYTES - 1
  };
  char *buffers[ITERATIONS];
  for (unsigned int i = 0; i < ITERATIONS; i++) {
    size_t size = 1 + random() % (i * 1000 + 1);
    UDS_ASSERT_SUCCESS(uds_allocate_cache_aligned(size, "test", &buffers[i]));
    CU_ASSERT_EQUAL(0, (uintptr_t) buffers[i] & LINE_MASK);
  }
  for (unsigned int i = 0; i < ITERATIONS; i++) {
    free(buffers[i]);
  }
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Bad strerror_r return", badStrerrorReturn },
  {"allocateCacheAligned",  testAllocateCacheAligned },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Misc_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
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
