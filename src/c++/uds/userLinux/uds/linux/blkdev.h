/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test requirements from linux/blkdev.h.
 *
 * $Id$
 */

#ifndef LINUX_BLKDEV_H
#define LINUX_BLKDEV_H

#include <linux/types.h>

#include "compiler.h"

#define SECTOR_SHIFT 9
#define SECTOR_SIZE 512

/* Defined in linux/fs.h but hacked for vdo unit testing */
struct inode {
  loff_t size;
};

/* Defined in linux/blk_types.h */
typedef uint32_t blk_status_t;
typedef unsigned int blk_qc_t;

struct bio;

struct block_device {
	int fd;
	dev_t bd_dev;

	/* This is only here for i_size_read(). */
	struct inode *bd_inode;
};

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


/**********************************************************************/
static inline struct block_device *
blkdev_get_by_dev(dev_t dev __always_unused,
		  fmode_t mode __always_unused,
		  void *holder __always_unused)
{
	/* This function will not get called in userspace. */
	return ERR_PTR(0);
}

/**********************************************************************/
struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder);

/**********************************************************************/
void blkdev_put(struct block_device *bdev, fmode_t mode);

/* Defined in linux/fs.h, but it's convenient to implement here. */
static inline loff_t i_size_read(const struct inode *inode)
{
        return (inode == NULL) ? (loff_t) SIZE_MAX : inode->size;
}

/*
 * Defined in linux/mount.h, but it's convenient to implement here. We
 * override this so we don't accidentally get a real device identifier.
 */
static inline dev_t name_to_dev_t(const char *name __always_unused)
{
	return 0;
}

#endif // LINUX_BLKDEV_H
