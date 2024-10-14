/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testBIO.h"

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/highmem.h>
#include <linux/kernel.h>

#include "memory-alloc.h"

#include "completion.h"
#include "types.h"
#include "vio.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

// Mocks of bio related functions in vdo/kernel/bio.c as well as the kernel
// itself.

void bio_init(struct bio          *bio,
              struct block_device *bdev,
              struct bio_vec      *table,
	      unsigned short       max_vecs,
              blk_opf_t            opf)
{
	bio->bi_next = NULL;
	bio->bi_bdev = bdev;
	bio->bi_opf = opf;
	bio->bi_flags = 0;
	bio->bi_ioprio = 0;
	bio->bi_status = 0;
	bio->bi_iter.bi_sector = 0;
	bio->bi_iter.bi_size = 0;
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_end_io = NULL;
	bio->bi_private = NULL;
	bio->bi_vcnt = 0;

	atomic_set(&bio->__bi_remaining, 1);
	atomic_set(&bio->__bi_cnt, 1);
	bio->bi_cookie = BLK_QC_T_NONE;

	bio->bi_max_vecs = max_vecs;
	bio->bi_io_vec = table;
	bio->bi_pool = NULL;
}

static

/**********************************************************************/
int __bio_clone(struct bio *bio, struct bio *bio_src, gfp_t gfp __attribute__((unused)))
{
  bio->bi_ioprio = bio_src->bi_ioprio;
  bio->bi_iter = bio_src->bi_iter;
  
  return 0;
}

int bio_init_clone(struct block_device *bdev,
                   struct bio *bio,
                   struct bio *bio_src,
                   gfp_t gfp)
{
  int ret;
  
  bio_init(bio, bdev, bio_src->bi_io_vec, 0, bio_src->bi_opf);
  ret = __bio_clone(bio, bio_src, gfp);
  if (ret)
	  bio_uninit(bio);
  return ret;
}

/**********************************************************************/
void __bio_add_page(struct bio *bio,
                    struct page *page,
		    unsigned int len,
		    unsigned int off)
{
  STATIC_ASSERT(PAGE_SIZE == VDO_BLOCK_SIZE);
  struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];

  bvec_set_page(bv, page, len, off);

  bio->bi_iter.bi_size += len;
  bio->bi_vcnt++;
}

int bio_add_page(struct bio *bio, struct page *page,
		 unsigned int len, unsigned int off)
{
  __bio_add_page(bio, page, len, off);
  return len;
}

/**********************************************************************/
void zero_fill_bio_iter(struct bio *bio, struct bvec_iter start)
{
  struct bio_vec bv;
  struct bvec_iter iter;
  
  __bio_for_each_segment(bv, bio, iter, start)
    memzero_bvec(&bv);
}

/**********************************************************************/
void bio_reset(struct bio *bio, struct block_device *bdev, unsigned int opf)
{
  void *context = bio->unitTestContext;
  memset(bio, 0, sizeof(struct bio));
  bio->unitTestContext = context;
  bio->bi_bdev = bdev;
  bio->bi_opf  = opf;
}

/**********************************************************************/
void bio_endio(struct bio *bio)
{
  bio->bi_end_io(bio);
}

/**********************************************************************/
void bio_uninit(struct bio *bio __attribute__((unused)))
{
  // nothing we need to do here.
}

/**********************************************************************/
void submit_bio_noacct(struct bio *bio)
{
  enqueueBIO(bio);
}

/**********************************************************************/
static void submit_bio_wait_endio(struct bio *bio)
{
  signalState(((struct vio *) bio->bi_private)->completion.parent);
}

/**********************************************************************/
int submit_bio_wait(struct bio *bio)
{
  bool done = false;
  struct vio vio;
  if (bio->bi_private == NULL) {
    CU_ASSERT_EQUAL(bio->bi_vcnt, 0);
    memset(&vio, 0, sizeof(struct vio));
    vio.bio = bio;
    vdo_initialize_completion(&vio.completion, vdo, VDO_TEST_COMPLETION);
    bio->bi_private = &vio;
  }

  bio->bi_end_io = submit_bio_wait_endio;
  bio->bi_flags = REQ_IDLE; /* Don't check the vdo admin state */
  ((struct vio *) bio->bi_private)->completion.parent = &done;
  enqueueBIO(bio);
  waitForState(&done);
  return bio->bi_status;
}

// Unit test only methods follow.

/**
 * Default endio function for a flush bio which just frees the bio.
 *
 * Implements bio_end_io_t
 **/
static void freeBIOEndio(struct bio *bio)
{
  vdo_free(bio);
}

/**********************************************************************/
struct bio *createFlushBIO(bio_end_io_t *endio)
{
  struct bio *bio;
  vdo_create_bio(&bio);
  bio->bi_opf          = REQ_PREFLUSH;
  bio->bi_end_io       = ((endio == NULL) ? freeBIOEndio : endio);
  bio->bi_iter.bi_size = 0;
  return bio;
}
