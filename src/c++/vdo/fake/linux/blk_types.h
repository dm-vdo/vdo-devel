/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test requirements from linux/bio.h.
 *
 * Copyright 2024 Red Hat
 *
 */

#ifndef LINUX_BLK_TYPES_H
#define LINUX_BLK_TYPES_H

#include <linux/atomic.h>
#include <linux/bvec.h>

struct bio;
typedef u32 __bitwise blk_opf_t;
typedef unsigned int blk_qc_t;
typedef u8 __bitwise blk_status_t;
typedef void (bio_end_io_t) (struct bio *);

#define BLK_QC_T_NONE -1U

/*
 * In the kernel, this is actually in blk-types.h which bio.h includes,
 * but this is expedient.
 *
 * Fields which are not used in unit tests are ifdefed out.
 */
struct bio {
        /* Extra field added for unit tests, will point to the enclosing BIO */
        void                    *unitTestContext;

	struct bio		*bi_next;	/* request queue link */
	struct block_device	*bi_bdev;
	blk_opf_t		bi_opf;		/* bottom bits REQ_OP, top bits
						 * req_flags.
						 */
	unsigned short		bi_flags;	/* BIO_* below */
	unsigned short		bi_ioprio;
	unsigned short		bi_write_hint;
	blk_status_t		bi_status;
	atomic_t		__bi_remaining;

	struct bvec_iter	bi_iter;

	union {
		/* for polled bios: */
		blk_qc_t	bi_cookie;
		/* for plugged zoned writes only: */
		unsigned int	__bi_nr_segments;
	};
	bio_end_io_t		*bi_end_io;

	void			*bi_private;
#ifdef CONFIG_BLK_CGROUP
	/*
	 * Represents the association of the css and request_queue for the bio.
	 * If a bio goes direct to device, it will not have a blkg as it will
	 * not have a request_queue associated with it.  The reference is put
	 * on release of the bio.
	 */
	struct blkcg_gq		*bi_blkg;
	struct bio_issue	bi_issue;
#ifdef CONFIG_BLK_CGROUP_IOCOST
	u64			bi_iocost_cost;
#endif
#endif

#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	struct bio_crypt_ctx	*bi_crypt_context;
#endif

	union {
#if defined(CONFIG_BLK_DEV_INTEGRITY)
		struct bio_integrity_payload *bi_integrity; /* data integrity */
#endif
	};

	unsigned short		bi_vcnt;	/* how many bio_vec's */

	/*
	 * Everything starting with bi_max_vecs will be preserved by bio_reset()
	 */

	unsigned short		bi_max_vecs;	/* max bvl_vecs we can hold */

	atomic_t		__bi_cnt;	/* pin count */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */

	struct bio_set		*bi_pool;

	/*
	 * We can inline a number of vecs at the end of the bio, to avoid
	 * double allocations for a small number of bio_vecs. This member
	 * MUST obviously be kept at the very end of the bio.
	 */
         struct bio_vec		 bi_inline_vecs[];
};

/*
 * bio flags
 */
enum {
	BIO_PAGE_PINNED,	/* Unpin pages in bio_release_pages() */
	BIO_CLONED,		/* doesn't own data */
	BIO_BOUNCED,		/* bio is a bounce bio */
	BIO_QUIET,		/* Make BIO Quiet */
	BIO_CHAIN,		/* chained bio, ->bi_remaining in effect */
	BIO_REFFED,		/* bio has elevated ->bi_cnt */
	BIO_BPS_THROTTLED,	/* This bio has already been subjected to
				 * throttling rules. Don't do it again. */
	BIO_TRACE_COMPLETION,	/* bio_endio() should trace the final completion
				 * of this bio. */
	BIO_CGROUP_ACCT,	/* has been accounted to a cgroup */
	BIO_QOS_THROTTLED,	/* bio went through rq_qos throttle path */
	BIO_QOS_MERGED,         /* but went through rq_qos merge path */
	BIO_REMAPPED,
	BIO_ZONE_WRITE_PLUGGING, /* bio handled through zone write plugging */
	BIO_EMULATES_ZONE_APPEND, /* bio emulates a zone append operation */
	BIO_FLAG_LAST
};

/*
 * Operations and flags common to the bio and request structures.
 * We use 8 bits for encoding the operation, and the remaining 24 for flags.
 *
 * The least significant bit of the operation number indicates the data
 * transfer direction:
 *
 *   - if the least significant bit is set transfers are TO the device
 *   - if the least significant bit is not set transfers are FROM the device
 *
 * If a operation does not transfer data the least significant bit has no
 * meaning.
 */
#define REQ_OP_BITS	8
#define REQ_OP_MASK	((1 << REQ_OP_BITS) - 1)
#define REQ_FLAG_BITS	24

enum req_op {
	/* read sectors from the device */
	REQ_OP_READ		= 0,
	/* write sectors to the device */
	REQ_OP_WRITE		= 1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH		= 2,
	/* discard sectors */
	REQ_OP_DISCARD		= 3,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE	= 5,
	/* write data at the current zone write pointer */
	REQ_OP_ZONE_APPEND	= 7,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES	= 9,
	/* Open a zone */
	REQ_OP_ZONE_OPEN	= 10,
	/* Close a zone */
	REQ_OP_ZONE_CLOSE	= 11,
	/* Transition a zone to full */
	REQ_OP_ZONE_FINISH	= 12,
	/* reset a zone write pointer */
	REQ_OP_ZONE_RESET	= 13,
	/* reset all the zone present on the device */
	REQ_OP_ZONE_RESET_ALL	= 15,

	/* Driver private requests */
	REQ_OP_DRV_IN		= 34,
	REQ_OP_DRV_OUT		= 35,

	REQ_OP_LAST		= 36,
};

/*
 * Request flags.  For use in the cmd_flags field of struct request, and in
 * bi_opf of struct bio.  Note that some flags are only valid in either one.
 */
enum req_flag_bits {
	__REQ_FAILFAST_DEV = REQ_OP_BITS, /* no driver retries of device errors */
	__REQ_FAILFAST_TRANSPORT, /* no driver retries of transport errors */
	__REQ_FAILFAST_DRIVER,	/* no driver retries of driver errors */
	__REQ_SYNC,		/* request is sync (sync write or read) */
	__REQ_META,		/* metadata io request */
	__REQ_PRIO,		/* boost priority in cfq */
	__REQ_NOMERGE,          /* don't touch this for merging */
	__REQ_IDLE,		/* anticipate more IO after this one */
	__REQ_INTEGRITY,	/* I/O includes block integrity payload */
	__REQ_FUA,		/* forced unit access */
	__REQ_PREFLUSH,		/* request for cache flush */
	__REQ_RAHEAD,		/* read ahead, can fail anytime */
	__REQ_BACKGROUND,       /* background IO */
	__REQ_NOWAIT,           /* Don't wait if request will block */
	__REQ_POLLED,           /* caller polls for completion using bio_poll */
	__REQ_ALLOC_CACHE,      /* allocate IO from cache if available */
	__REQ_SWAP,             /* swap I/O */
	__REQ_DRV,              /* for driver use */
	__REQ_FS_PRIVATE,       /* for file system (submitter) use */

	/*
	 * Command specific flags, keep last:
	 */
	/* for REQ_OP_WRITE_ZEROES: */
	__REQ_NOUNMAP,		/* do not free blocks when zeroing */

	__REQ_NR_BITS,		/* stops here */
};

#define REQ_FAILFAST_DEV	(1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT	(1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER	(1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC		(1ULL << __REQ_SYNC)
#define REQ_META		(1ULL << __REQ_META)
#define REQ_PRIO		(1ULL << __REQ_PRIO)
#define REQ_NOMERGE		(1ULL << __REQ_NOMERGE)
#define REQ_IDLE		(1ULL << __REQ_IDLE)
#define REQ_INTEGRITY		(1ULL << __REQ_INTEGRITY)
#define REQ_FUA                 (1ULL << __REQ_FUA)
#define REQ_PREFLUSH		(1ULL << __REQ_PREFLUSH)
#define REQ_RAHEAD		(1ULL << __REQ_RAHEAD)
#define REQ_BACKGROUND		(1ULL << __REQ_BACKGROUND)
#define REQ_NOWAIT		(1ULL << __REQ_NOWAIT)
#define REQ_POLLED		(1ULL << __REQ_POLLED)
#define REQ_ALLOC_CACHE		(1ULL << __REQ_ALLOC_CACHE)
#define REQ_SWAP		(1ULL << __REQ_SWAP)
#define REQ_DRV                 (1ULL << __REQ_DRV)
#define REQ_FS_PRIVATE		(1ULL << __REQ_FS_PRIVATE)

#define REQ_NOUNMAP		(1ULL << __REQ_NOUNMAP)

#define REQ_FAILFAST_MASK \
	(REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER)

/* This mask is used for both bio and request merge checking */
#define REQ_NOMERGE_FLAGS \
	(REQ_NOMERGE | REQ_PREFLUSH | REQ_FUA )

static inline enum req_op bio_op(const struct bio *bio)
{
	return bio->bi_opf & REQ_OP_MASK;
}

static inline bool op_is_write(blk_opf_t op)
{
	return !!(op & 1);
}

#endif // LINUX_BLK_TYPES_H

