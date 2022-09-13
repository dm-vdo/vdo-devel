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
#include <linux/kernel.h>

#include "memory-alloc.h"

#include "bio.h"
#include "completion.h"
#include "types.h"
#include "vio.h"

#include "asyncLayer.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

// Mocks of bio related functions in vdo/kernel/bio.c as well as the kernel
// itself.

#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0))
#endif

#if (!USE_ALTERNATE)
static
#endif // not USE_ALTERNATE
/**********************************************************************/
void __bio_clone_fast(struct bio *bio, struct bio *bio_src)
{
  bio->bi_bdev   = bio_src->bi_bdev;
  bio->bi_opf    = bio_src->bi_opf;
  bio->bi_iter   = bio_src->bi_iter;
  bio->bi_io_vec = bio_src->bi_io_vec;
}

#if (!USE_ALTERNATE)
int bio_init_clone(struct block_device *bdev __attribute__((unused)),
                   struct bio *bio,
                   struct bio *bio_src,
                   gfp_t gfp __attribute__((unused)))
{
  __bio_clone_fast(bio, bio_src);
  return 1;
}
#endif // not USE_ALTERNATE

/**********************************************************************/
int bio_add_page(struct bio *bio,
                 struct page *page,
		 unsigned int len,
                 unsigned int offset)
{
  struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];

  bv->bv_page = page;
  bv->bv_offset = offset;
  bv->bv_len = len;

  bio->bi_iter.bi_size += len;
  bio->bi_vcnt++;

  return len;
}

/**********************************************************************/
void zero_fill_bio(struct bio *bio)
{
  if (bio->bi_vcnt == 0) {
    return;
  }

  CU_ASSERT_EQUAL(bio->bi_vcnt, 1);
  struct bio_vec *bvec = bio->bi_io_vec;
  memset(bvec->bv_page, 0, bvec->bv_len);
}

#if USE_ALTERNATE
/**********************************************************************/
void bio_reset(struct bio *bio)
{
  void *context = bio->unitTestContext;
  memset(bio, 0, sizeof(struct bio));
  bio->unitTestContext = context;
}
#else // not USE_ALTERNATE
/**********************************************************************/
void bio_reset(struct bio *bio, struct block_device *bdev, unsigned int opf)
{
  void *context = bio->unitTestContext;
  memset(bio, 0, sizeof(struct bio));
  bio->unitTestContext = context;
  bio->bi_bdev = bdev;
  bio->bi_opf  = opf;
}
#endif // USE_ALTERNATE

/**********************************************************************/
void bio_uninit(struct bio *bio __attribute__((unused)))
{
  // nothing we need to do here.
}

/**********************************************************************/
blk_qc_t submit_bio_noacct(struct bio *bio)
{
  enqueueBIO(bio);

  // Nothing looks at this return value.
  return 0;
}

// Unit test only methods follow.

/**
 * Default endio function for a flush bio which just frees the bio.
 *
 * Implements bio_end_io_t
 **/
static void freeBIOEndio(struct bio *bio)
{
  UDS_FREE(bio);
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
