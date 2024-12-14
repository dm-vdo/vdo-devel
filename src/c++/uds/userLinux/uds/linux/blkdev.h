/*
 * Unit test requirements from linux/blkdev.h and other kernel headers.
 */

#ifndef LINUX_BLKDEV_H
#define LINUX_BLKDEV_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <errno.h>
#include <stdio.h>

#define SECTOR_SHIFT    9
#define SECTOR_SIZE   512
#define BDEVNAME_SIZE  32 /* Largest string for a blockdev identifier */

/* Defined in linux/kdev_t.h */
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)

#define MAJOR(dev) ((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned int) ((dev) & MINORMASK))

#define format_dev_t(buffer, dev)				\
	sprintf(buffer, "%u:%u", MAJOR(dev), MINOR(dev))

/* Defined in linux/blk_types.h */
typedef u32 __bitwise blk_opf_t;
typedef unsigned int  blk_qc_t;

/* Defined in linux/gfp_types.h */
typedef unsigned int  gfp_t;
#define GFP_KERNEL 1
#define GFP_NOWAIT 2
#define GFP_NOIO   3

typedef u8 __bitwise  blk_status_t;
#define BLK_STS_OK 0
#define BLK_STS_NOSPC    ((blk_status_t)3)
#define BLK_STS_RESOURCE ((blk_status_t)9)
#define BLK_STS_IOERR    ((blk_status_t)10)

/* hack for vdo, don't use elsewhere */
#define BLK_STS_VDO_INJECTED ((blk_status_t)31)

struct bio;

struct block_device {
	int fd;
	dev_t bd_dev;

	/* This is only here for bdev_nr_bytes(). */
	loff_t size;
};

/* Defined in linux/blk-core.c */
static const struct {
	int         error;
	const char *name;
} blk_errors[] = {
	[BLK_STS_OK]           = { 0,		"" },
	[BLK_STS_NOSPC]        = { -ENOSPC,	"critical space allocation" },
	[BLK_STS_RESOURCE]     = { -ENOMEM,	"kernel resource" },
	
	/* error specifically for VDO unit tests */
	[BLK_STS_VDO_INJECTED] = { 31,		"vdo injected error" },
	/* everything else not covered above: */
	[BLK_STS_IOERR]        = { -EIO,	"I/O" },
};

/**********************************************************************/
static inline int blk_status_to_errno(blk_status_t status)
{
	int idx = (int) status;

	return blk_errors[idx].error;
}

/**********************************************************************/
static inline blk_status_t errno_to_blk_status(int error)
{
	unsigned int i;
	
	for (i = 0; i < ARRAY_SIZE(blk_errors); i++) {
		if (blk_errors[i].error == error)
			return (blk_status_t)i;
	}

	return BLK_STS_IOERR;
}

/**********************************************************************/
void submit_bio_noacct(struct bio *bio);

/**********************************************************************/
static inline loff_t bdev_nr_bytes(struct block_device *bdev)
{
	return bdev->size;
}

/**
 * blkdev_issue_zeroout - zero-fill a block range
 * @bdev:	blockdev to write
 * @sector:	start sector
 * @nr_sects:	number of sectors to write
 * @gfp_mask:	memory allocation flags (for bio_alloc)
 * @flags:	controls detailed behavior
 *
 * Description:
 *  Zero-fill a block range, either using hardware offload or by explicitly
 *  writing zeroes to the device.  See __blkdev_issue_zeroout() for the
 *  valid values for %flags.
 */
int blkdev_issue_zeroout(struct block_device *bdev, sector_t sector,
			 sector_t nr_sects, gfp_t gfp_mask, unsigned flags);

#endif // LINUX_BLKDEV_H
