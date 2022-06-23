/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "instance-number.h"

#include "vdoAsserts.h"

static unsigned int selected[] = { 5, 50, 500, 1050, 1099, 1199 };

/**********************************************************************/
static void allocateRange(unsigned int start, unsigned int end)
{
  unsigned int instance;

  // Allocate and release a range of instance numbers, none should get reused.
  for (unsigned int i = start; i < end; i++) {
    VDO_ASSERT_SUCCESS(vdo_allocate_instance(&instance));
    CU_ASSERT_EQUAL(i, instance);
    vdo_release_instance(i);
  }

  // Allocate them again, they should all get reused.
  for (unsigned int i = start; i < end; i++) {
    VDO_ASSERT_SUCCESS(vdo_allocate_instance(&instance));
    CU_ASSERT_EQUAL(i, instance);
  }
}

/**********************************************************************/
static void reallocateSelected(unsigned int n)
{
  for (unsigned int i = n; i > 0; i--) {
    vdo_release_instance(selected[i - 1]);
  }

  for (unsigned int i = 0; i < n; i++) {
    unsigned int instance;
    VDO_ASSERT_SUCCESS(vdo_allocate_instance(&instance));
    CU_ASSERT_EQUAL(selected[i], instance);
  }
}

/**********************************************************************/
static void testInstanceNumbers(void)
{
  // Re-initialize in case other tests have been run in this process.
  vdo_clean_up_instance_number_tracking();
  vdo_initialize_instance_number_tracking();

  // Allocate and reallocate the first 1000 in order.
  allocateRange(0, 1000);

  // Now release a few and see that they are reused.
  reallocateSelected(3);

  // Allocate and release 100 more and see that they are all new and in order.
  allocateRange(1000, 1100);

  // Release a few and see that they are reused.
  reallocateSelected(4);

  // Allocate 1 more batch and check selectivity again.
  allocateRange(1100, 1200);
  reallocateSelected(6);

  // Allocate and release a range of instance numbers, none should get reused.
  for (unsigned int i = 0; i < 1200; i++) {
    vdo_release_instance(i);
  }
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "instance numbers", testInstanceNumbers },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name  = "Instance number tests (InstanceNumber_t1)",
  .tests = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
