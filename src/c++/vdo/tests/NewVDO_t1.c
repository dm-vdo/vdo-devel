/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "memory-alloc.h"

#include "slab-depot.h"
#include "vdo.h"
#include "vdoConfig.h"

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void verifySlabSummary(void)
{
  struct block_allocator *allocator = &vdo->depot->allocators[0];

  struct slab_status *statuses;
  VDO_ASSERT_SUCCESS(get_slab_statuses(allocator, &statuses));

  for (size_t i = allocator->slab_count; i > 0; i--) {
    struct slab_status *status = statuses + (i - 1);
    CU_ASSERT_EQUAL(allocator->slab_count - i, status->slab_number);
    CU_ASSERT_EQUAL(true,  status->is_clean);
    CU_ASSERT_NOT_EQUAL(0, status->emptiness);
    CU_ASSERT_EQUAL(0, allocator->summary_entries[status->slab_number].tail_block_offset);
  }

  UDS_FREE(statuses);
}

/**********************************************************************/
static void testNewVDOSlabStatus(void)
{
  verifySlabSummary();

  // Now destroy that vdo without saving.
  crashVDO();
  startVDO(VDO_DIRTY);
  verifySlabSummary();
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "new vdo slab status", testNewVDOSlabStatus },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "NewVDO_t1",
  .initializer = initializeDefaultVDOTest,
  .cleaner     = tearDownVDOTest,
  .tests       = tests
};

/**********************************************************************/
CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
