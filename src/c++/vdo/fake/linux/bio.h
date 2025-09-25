/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/bio.h.
 *
 * Copyright (C) 2001 Jens Axboe <axboe@suse.de>
 * Copyright 2024 Red Hat
 *
 */

#ifndef __LINUX_BIO_H
#define __LINUX_BIO_H

/* struct bio, bio_vec and BIO_* flags are defined in blk_types.h */
#include <linux/blk_types.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/unaligned.h>
#include <linux/version.h>

#define BIO_MAX_VECS		256U

#define bio_prio(bio)			(bio)->bi_ioprio
#define bio_set_prio(bio, prio)		((bio)->bi_ioprio = prio)

#define bio_iter_iovec(bio, iter)				\
	bvec_iter_bvec((bio)->bi_io_vec, (iter))
#define bio_iter_offset(bio, iter)				\
	bvec_iter_offset((bio)->bi_io_vec, (iter))

#define bio_offset(bio)		bio_iter_offset((bio), (bio)->bi_iter)
#define bio_iovec(bio)		bio_iter_iovec((bio), (bio)->bi_iter)

/*
 * Return the data direction, READ or WRITE.
 */
#define bio_data_dir(bio) \
	(op_is_write(bio_op(bio)) ? WRITE : READ)

static inline bool bio_no_advance_iter(const struct bio *bio)
{
	return bio_op(bio) == REQ_OP_DISCARD ||
	       bio_op(bio) == REQ_OP_SECURE_ERASE ||
	       bio_op(bio) == REQ_OP_WRITE_ZEROES;
}

static inline void bio_advance_iter(const struct bio *bio,
				    struct bvec_iter *iter, unsigned int bytes)
{
	iter->bi_sector += bytes >> 9;

	if (bio_no_advance_iter(bio))
		iter->bi_size -= bytes;
	else
		bvec_iter_advance(bio->bi_io_vec, iter, bytes);
		/* TODO: It is reasonable to complete bio with error here. */
}

/* @bytes should be less or equal to bvec[i->bi_idx].bv_len */
static inline void bio_advance_iter_single(const struct bio *bio,
					   struct bvec_iter *iter,
					   unsigned int bytes)
{
	iter->bi_sector += bytes >> 9;

	if (bio_no_advance_iter(bio))
		iter->bi_size -= bytes;
	else
		bvec_iter_advance_single(bio->bi_io_vec, iter, bytes);
}

void __bio_advance(struct bio *, unsigned bytes);

/**
 * bio_advance - increment/complete a bio by some number of bytes
 * @bio:	bio to advance
 * @nbytes:	number of bytes to complete
 *
 * This updates bi_sector, bi_size and bi_idx; if the number of bytes to
 * complete doesn't align with a bvec boundary, then bv_len and bv_offset will
 * be updated on the last bvec as well.
 *
 * @bio will then represent the remaining, uncompleted portion of the io.
 */
static inline void bio_advance(struct bio *bio, unsigned int nbytes)
{
	if (nbytes == bio->bi_iter.bi_size) {
		bio->bi_iter.bi_size = 0;
		return;
	}
	__bio_advance(bio, nbytes);
}

#define __bio_for_each_segment(bvl, bio, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bio_iter_iovec((bio), (iter))), 1);		\
	     bio_advance_iter_single((bio), &(iter), (bvl).bv_len))

#define bio_for_each_segment(bvl, bio, iter)				\
	__bio_for_each_segment(bvl, bio, iter, (bio)->bi_iter)

static inline bool bio_flagged(struct bio *bio, unsigned int bit)
{
	return bio->bi_flags & (1U << bit);
}

static inline void bio_clear_flag(struct bio *bio, unsigned int bit)
{
	bio->bi_flags &= ~(1U << bit);
}

enum bip_flags {
	BIP_BLOCK_INTEGRITY	= 1 << 0, /* block layer owns integrity data */
	BIP_MAPPED_INTEGRITY	= 1 << 1, /* ref tag has been remapped */
	BIP_CTRL_NOCHECK	= 1 << 2, /* disable HBA integrity checking */
	BIP_DISK_NOCHECK	= 1 << 3, /* disable disk integrity checking */
	BIP_IP_CHECKSUM		= 1 << 4, /* IP checksum */
	BIP_INTEGRITY_USER	= 1 << 5, /* Integrity payload is user address */
	BIP_COPY_USER		= 1 << 6, /* Kernel bounce buffer in use */
};

#if defined(CONFIG_BLK_DEV_INTEGRITY)
/*
 * bio integrity payload
 */
struct bio_integrity_payload {
	struct bio		*bip_bio;	/* parent bio */

	struct bvec_iter	bip_iter;

	unsigned short		bip_vcnt;	/* # of integrity bio_vecs */
	unsigned short		bip_max_vcnt;	/* integrity bio_vec slots */
	unsigned short		bip_flags;	/* control flags */

	struct bvec_iter	bio_iter;	/* for rewinding parent bio */

	struct work_struct	bip_work;	/* I/O completion */

	struct bio_vec		*bip_vec;
	struct bio_vec		bip_inline_vecs[];/* embedded bvec array */
};
#endif

struct bio *bio_alloc_clone(struct block_device *bdev, struct bio *bio_src,
		gfp_t gfp, struct bio_set *bs);
int bio_init_clone(struct block_device *bdev, struct bio *bio,
                   struct bio *bio_src, gfp_t gfp);

extern void bio_endio(struct bio *);

extern int submit_bio_wait(struct bio *bio);
#ifndef VDO_UPSTREAM
#undef VDO_USE_NEXT
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_MINOR) && (RHEL_MINOR < 50)
#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(10, 2))
#define VDO_USE_NEXT
#endif
#else
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0))
#define VDO_USE_NEXT
#endif
#endif /* RHEL_RELEASE_CODE */
#endif /* VDO_UPSTREAM */
#ifdef VDO_USE_NEXT
static inline struct bio_vec *bio_inline_vecs(struct bio *bio)
{
	return (struct bio_vec *)(bio + 1);
}
#endif /* VDO_USE_NEXT */
void bio_init(struct bio *bio, struct block_device *bdev, struct bio_vec *table,
	      unsigned short max_vecs, blk_opf_t opf);
extern void bio_uninit(struct bio *);
void bio_reset(struct bio *bio, struct block_device *bdev, blk_opf_t opf);

int __must_check bio_add_page(struct bio *bio, struct page *page, unsigned len,
			      unsigned off);
void __bio_add_page(struct bio *bio, struct page *page,
		unsigned int len, unsigned int off);

void zero_fill_bio_iter(struct bio *bio, struct bvec_iter iter);

static inline void zero_fill_bio(struct bio *bio)
{
	zero_fill_bio_iter(bio, bio->bi_iter);
}

#ifdef CONFIG_BLK_CGROUP
void bio_associate_blkg(struct bio *bio);
#else	/* CONFIG_BLK_CGROUP */
static inline void bio_associate_blkg(struct bio *bio) { }
#endif	/* CONFIG_BLK_CGROUP */

static inline void bio_set_dev(struct bio *bio, struct block_device *bdev)
{
	bio_clear_flag(bio, BIO_REMAPPED);
	if (bio->bi_bdev != bdev)
		bio_clear_flag(bio, BIO_BPS_THROTTLED);
	bio->bi_bdev = bdev;
	bio_associate_blkg(bio);
}

/*
 * BIO list management for use by remapping drivers (e.g. DM or MD) and loop.
 *
 * A bio_list anchors a singly-linked list of bios chained through the bi_next
 * member of the bio.  The bio_list also caches the last list member to allow
 * fast access to the tail.
 */
struct bio_list {
	struct bio *head;
	struct bio *tail;
};

static inline int bio_list_empty(const struct bio_list *bl)
{
	return bl->head == NULL;
}

static inline void bio_list_init(struct bio_list *bl)
{
	bl->head = bl->tail = NULL;
}

#define bio_list_for_each(bio, bl) \
	for (bio = (bl)->head; bio; bio = bio->bi_next)

static inline unsigned bio_list_size(const struct bio_list *bl)
{
	unsigned sz = 0;
	struct bio *bio;

	bio_list_for_each(bio, bl)
		sz++;

	return sz;
}

static inline void bio_list_add(struct bio_list *bl, struct bio *bio)
{
	bio->bi_next = NULL;

	if (bl->tail)
		bl->tail->bi_next = bio;
	else
		bl->head = bio;

	bl->tail = bio;
}

static inline void bio_list_merge(struct bio_list *bl, struct bio_list *bl2)
{
	if (!bl2->head)
		return;

	if (bl->tail)
		bl->tail->bi_next = bl2->head;
	else
		bl->head = bl2->head;

	bl->tail = bl2->tail;
}

static inline void bio_list_merge_init(struct bio_list *bl,
				       struct bio_list *bl2)
{
	bio_list_merge(bl, bl2);
	bio_list_init(bl2);
}

static inline void bio_list_merge_head(struct bio_list *bl,
				       struct bio_list *bl2)
{
	if (!bl2->head)
		return;

	if (bl->head)
		bl2->tail->bi_next = bl->head;
	else
		bl->tail = bl2->tail;

	bl->head = bl2->head;
}

static inline struct bio *bio_list_peek(struct bio_list *bl)
{
	return bl->head;
}

static inline struct bio *bio_list_pop(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	if (bio) {
		bl->head = bl->head->bi_next;
		if (!bl->head)
			bl->tail = NULL;

		bio->bi_next = NULL;
	}

	return bio;
}

static inline struct bio *bio_list_get(struct bio_list *bl)
{
	struct bio *bio = bl->head;

	bl->head = bl->tail = NULL;

	return bio;
}

#endif /* __LINUX_BIO_H */
