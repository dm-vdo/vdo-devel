/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/highmem.h>
#include <linux/kernel.h>

#include "types.h"

#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

int blkdev_issue_zeroout(__attribute__((unused)) struct block_device *bdev,
			 __attribute__((unused)) sector_t sector,
			 __attribute__((unused)) sector_t nr_sects,
			 __attribute__((unused)) gfp_t gfp_mask,
			 __attribute__((unused)) unsigned flags)
{
  return VDO_SUCCESS;
}
