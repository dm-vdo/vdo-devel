/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include "asyncLayer.h"

#include "slab-depot.h"
#include "slab-summary.h"
#include "vdo.h"
#include "vdoConfig.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
static void verifySlabSummary(struct vdo *vdo)
{
  struct slab_summary_zone *summaryZone = vdo->depot->slab_summary->zones[0];
  slab_count_t              slabCount   = vdo->depot->slab_count;
  struct slab_status statuses[slabCount];
  vdo_get_summarized_slab_statuses(summaryZone, slabCount, statuses);

  for (size_t i = 0; i < slabCount; ++i) {
    struct slab_status *status = &statuses[i];
    CU_ASSERT_EQUAL(i,     status->slab_number);
    CU_ASSERT_EQUAL(true,  status->is_clean);
    CU_ASSERT_NOT_EQUAL(0, status->emptiness);
    CU_ASSERT_EQUAL(0, vdo_get_summarized_tail_block_offset(summaryZone, i));
  }
}

/**********************************************************************/
static void testNewVDOSlabStatus(void)
{
  verifySlabSummary(vdo);

  // Now destroy that vdo without saving.
  crashVDO();
  startVDO(VDO_DIRTY);
  verifySlabSummary(vdo);
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
