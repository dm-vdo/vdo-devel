/*
 * FOR INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * $Id$
 */

#include <linux/dm-kcopyd.h>

#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "kernel-types.h"
#include "types.h"

#include "physicalLayer.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

struct dm_kcopyd_client {
  char dummy;
};

struct dm_kcopyd_client CLIENT;

/**********************************************************************/
void dm_kcopyd_copy(struct dm_kcopyd_client *kc,
		    struct dm_io_region *from,
		    unsigned int num_dests,
		    struct dm_io_region *dests,
		    unsigned int flags,
		    dm_kcopyd_notify_fn fn,
		    void *context)
{
  CU_ASSERT_PTR_EQUAL(kc, &CLIENT);

  // Can't handle flags in this fake.
  CU_ASSERT_EQUAL(flags, 0);

  // Can't handle multiple destinations.
  CU_ASSERT_EQUAL(num_dests, 1);

  char                    buffer[VDO_BLOCK_SIZE];
  block_count_t           blocks  = from->count / VDO_SECTORS_PER_BLOCK;
  physical_block_number_t fromPBN = from->sector / VDO_SECTORS_PER_BLOCK;
  physical_block_number_t toPBN   = dests->sector / VDO_SECTORS_PER_BLOCK;
  for (block_count_t i = 0; i < blocks; i++) {
    VDO_ASSERT_SUCCESS(layer->reader(layer, fromPBN++, 1, buffer));
    VDO_ASSERT_SUCCESS(layer->writer(layer, toPBN++, 1, buffer));
  }

  fn(0, 0, context);
}

/**********************************************************************/
void dm_kcopyd_zero(struct dm_kcopyd_client *kc,
		    unsigned num_dests,
		    struct dm_io_region *dests,
		    unsigned flags,
		    dm_kcopyd_notify_fn fn,
		    void *context)
{
  CU_ASSERT_PTR_EQUAL(kc, &CLIENT);

  // Can't handle flags in this fake.
  CU_ASSERT_EQUAL(flags, 0);

  // Can't handle multiple destinations.
  CU_ASSERT_EQUAL(num_dests, 1);

  char buffer[VDO_BLOCK_SIZE];
  memset(buffer, 0, VDO_BLOCK_SIZE);

  block_count_t           blocks = dests->count / VDO_SECTORS_PER_BLOCK;
  physical_block_number_t pbn    = dests->sector / VDO_SECTORS_PER_BLOCK;
  for (block_count_t i = 0; i < blocks; i++) {
    VDO_ASSERT_SUCCESS(layer->writer(layer, pbn++, 1, buffer));
  }

  fn(0, 0, context);
}

/**********************************************************************/
struct dm_kcopyd_client *
dm_kcopyd_client_create(struct dm_kcopyd_throttle *throttle)
{
  // Can't handle throttles.
  CU_ASSERT_PTR_NULL(throttle);
  return &CLIENT;
}

/**********************************************************************/
void dm_kcopyd_client_destroy(struct dm_kcopyd_client *kc)
{
  CU_ASSERT_PTR_EQUAL(kc, &CLIENT);
}
