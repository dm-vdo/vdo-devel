/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test requirements from linux/blkdev.h.
 *
 * $Id$
 */

#ifndef LINUX_BLKDEV_H
#define LINUX_BLKDEV_H

#include <linux/blk_types.h>

/**********************************************************************/
static inline int blk_status_to_errno(blk_status_t status)
{
  return (int) status;
}

/**********************************************************************/
static inline blk_status_t errno_to_blk_status(int error)
{
  return (blk_status_t) error;
}

/**********************************************************************/
blk_qc_t submit_bio_noacct(struct bio *bio);

#endif // LINUX_BLKDEV_H
