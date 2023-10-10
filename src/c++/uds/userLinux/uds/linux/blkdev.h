/*
 * For INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test requirements from linux/blkdev.h.
 *
 * $Id$
 */

#ifndef LINUX_BLKDEV_H
#define LINUX_BLKDEV_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>
#include <linux/version.h>
#include <stdio.h>

#define SECTOR_SHIFT    9
#define SECTOR_SIZE   512
#define BDEVNAME_SIZE  32 /* Largest string for a blockdev identifier */

/* Defined in linux/kdev_t.h */
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)

#define MAJOR(dev) ((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi) (((ma) << MINORBITS) | (mi))

#define format_dev_t(buffer, dev)				\
	sprintf(buffer, "%u:%u", MAJOR(dev), MINOR(dev))

#undef VDO_USE_ALTERNATE
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0))
#define VDO_USE_ALTERNATE
#endif

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

#ifndef VDO_USE_ALTERNATE
struct blk_holder_ops {
	void (*mark_dead)(struct block_device *bdev);
};

#endif /* !VDO_USE_ALTERNATE */
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
#ifdef VDO_USE_ALTERNATE
static inline struct block_device *
blkdev_get_by_dev(dev_t dev __always_unused,
		  fmode_t mode __always_unused,
		  void *holder __always_unused)
#else
static inline struct block_device *
blkdev_get_by_dev(dev_t dev __always_unused,
		  fmode_t mode __always_unused,
		  void *holder __always_unused,
		  const struct blk_holder_ops *hops __always_unused)
#endif /* VDO_USE_ALTERNATE */
{
	/* This function will not get called in userspace. */
	return 0;
}

/**********************************************************************/
#ifdef VDO_USE_ALTERNATE
struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder);
#else
struct block_device *blkdev_get_by_path(const char *path,
					fmode_t mode,
					void *holder,
					const struct blk_holder_ops *hops __always_unused);
#endif /* VDO_USE_ALTERNATE */

/**********************************************************************/
#ifdef VDO_USE_ALTERNATE
void blkdev_put(struct block_device *bdev, fmode_t mode);
#else
void blkdev_put(struct block_device *bdev, void *holder);
#endif /* VDO_USE_ALTERNATE */

/* Defined in linux/fs.h, but it's convenient to implement here. */
static inline loff_t i_size_read(const struct inode *inode)
{
        return (inode == NULL) ? (loff_t) SIZE_MAX : inode->size;
}

#endif // LINUX_BLKDEV_H
