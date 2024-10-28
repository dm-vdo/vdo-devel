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
#include "vio.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] = {0};
#define ZERO_PAGE(vaddr) ((void)(vaddr),virt_to_page(empty_zero_page))

static int __blkdev_issue_zero_pages(struct block_device *bdev,
		sector_t sector, sector_t nr_sects,
		struct bio **biop)
{
	int result = 0;
	struct bio *bio = *biop;
	unsigned int bi_size = 0;
	unsigned int size;

	result = vdo_create_bio(&bio);
	if (result != VDO_SUCCESS) {
		return result;
	}
	bio_init(bio, bdev, NULL, 0, REQ_OP_WRITE);

	while (nr_sects != 0) {
		bio->bi_iter.bi_sector = sector;
		while (nr_sects != 0) {
			size = min((sector_t) PAGE_SIZE, nr_sects << 9);
			bi_size = bio_add_page(bio, ZERO_PAGE(0), size, 0);
			nr_sects -= bi_size >> 9;
			sector += bi_size >> 9;
			if (bi_size < size) {
				break;
			}
		}
	}

	*biop = bio;
	return VDO_SUCCESS;
}

int blkdev_issue_zeroout(struct block_device *bdev,
			 sector_t sector,
			 sector_t nr_sects,
			 __attribute__((unused)) gfp_t gfp_mask,
			 __attribute__((unused)) unsigned flags)
{
	int result = 0;
	struct bio *bio;

	bio = NULL;
	result = __blkdev_issue_zero_pages(bdev, sector, nr_sects, &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}
	result = submit_bio_wait(bio);
	vdo_free_bio(bio);
	return result;
}
