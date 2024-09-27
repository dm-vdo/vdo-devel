/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copied from linux/bvec.h
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 * Copyright 2024 Red Hat
 *
 */

#ifndef __LINUX_BVEC_H
#define __LINUX_BVEC_H

#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/linuxTypes.h>

/**
 * struct bio_vec - a contiguous range of physical memory addresses
 * @bv_page:   First page associated with the address range.
 * @bv_len:    Number of bytes in the address range.
 * @bv_offset: Start of the address range relative to the start of @bv_page.
 *
 * The following holds for a bvec if n * PAGE_SIZE < bv_offset + bv_len:
 *
 *   nth_page(@bv_page, n) == @bv_page + n
 *
 * This holds because page_is_mergeable() checks the above property.
 */
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
	unsigned int	bv_offset;
};

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte
						   sectors */
	unsigned int		bi_size;	/* residual I/O count */

	unsigned int		bi_idx;		/* current index into bvl_vec */

	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
} __packed __aligned(4);

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

/* multi-page (mp_bvec) helpers */
#define mp_bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define mp_bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define mp_bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define mp_bvec_iter_page_idx(bvec, iter)			\
	(mp_bvec_iter_offset((bvec), (iter)) / PAGE_SIZE)

/* For building single-page bvec in flight */
 #define bvec_iter_offset(bvec, iter)				\
	(mp_bvec_iter_offset((bvec), (iter)) % PAGE_SIZE)

#define bvec_iter_len(bvec, iter)				\
	min_t(unsigned, mp_bvec_iter_len((bvec), (iter)),		\
	      PAGE_SIZE - bvec_iter_offset((bvec), (iter)))

#define bvec_iter_page(bvec, iter)				\
	(mp_bvec_iter_page((bvec), (iter)) +			\
	 mp_bvec_iter_page_idx((bvec), (iter)))

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

/**
 * bvec_set_page - initialize a bvec based off a struct page
 * @bv:		bvec to initialize
 * @page:	page the bvec should point to
 * @len:	length of the bvec
 * @offset:	offset into the page
 */
static inline void bvec_set_page(struct bio_vec *bv, struct page *page,
				 unsigned int len, unsigned int offset)
{
	bv->bv_page = page;
	bv->bv_len = len;
	bv->bv_offset = offset;
}

static inline bool bvec_iter_advance(const struct bio_vec *bv,
		struct bvec_iter *iter, unsigned bytes)
{
	unsigned int idx = iter->bi_idx;

	if (WARN_ONCE(bytes > iter->bi_size,
		     "Attempted to advance past end of bvec iter\n")) {
		iter->bi_size = 0;
		return false;
	}

	iter->bi_size -= bytes;
	bytes += iter->bi_bvec_done;

	while (bytes && bytes >= bv[idx].bv_len) {
		bytes -= bv[idx].bv_len;
		idx++;
	}

	iter->bi_idx = idx;
	iter->bi_bvec_done = bytes;
	return true;
}

/*
 * A simpler version of bvec_iter_advance(), @bytes should not span
 * across multiple bvec entries, i.e. bytes <= bv[i->bi_idx].bv_len
 */
static inline void bvec_iter_advance_single(const struct bio_vec *bv,
				struct bvec_iter *iter, unsigned int bytes)
{
	unsigned int done = iter->bi_bvec_done + bytes;

	if (done == bv[iter->bi_idx].bv_len) {
		done = 0;
		iter->bi_idx++;
	}
	iter->bi_bvec_done = done;
	iter->bi_size -= bytes;
}

/**
 * memcpy_from_bvec - copy data from a bvec
 * @bvec: bvec to copy from
 *
 * Must be called on single-page bvecs only.
 */
static inline void memcpy_from_bvec(char *to, struct bio_vec *bvec)
{
	memcpy_from_page(to, bvec->bv_page, bvec->bv_offset, bvec->bv_len);
}

/**
 * memcpy_to_bvec - copy data to a bvec
 * @bvec: bvec to copy to
 *
 * Must be called on single-page bvecs only.
 */
static inline void memcpy_to_bvec(struct bio_vec *bvec, const char *from)
{
	memcpy_to_page(bvec->bv_page, bvec->bv_offset, from, bvec->bv_len);
}

/**
 * memzero_bvec - zero all data in a bvec
 * @bvec: bvec to zero
 *
 * Must be called on single-page bvecs only.
 */
static inline void memzero_bvec(struct bio_vec *bvec)
{
	memzero_page(bvec->bv_page, bvec->bv_offset, bvec->bv_len);
}

#endif /* __LINUX_BVEC_H */
