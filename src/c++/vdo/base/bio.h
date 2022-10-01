/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BIO_H
#define BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

#include "kernel-types.h"

/* Keep struct bio statistics atomically */
struct atomic_bio_stats {
	atomic64_t read; /* Number of not REQ_WRITE bios */
	atomic64_t write; /* Number of REQ_WRITE bios */
	atomic64_t discard; /* Number of REQ_DISCARD bios */
	atomic64_t flush; /* Number of REQ_FLUSH bios */
	atomic64_t empty_flush; /* Number of REQ_PREFLUSH bios without data */
	atomic64_t fua; /* Number of REQ_FUA bios */
};

void vdo_bio_copy_data_in(struct bio *bio, char *data_ptr);
void vdo_bio_copy_data_out(struct bio *bio, char *data_ptr);

physical_block_number_t __must_check pbn_from_vio_bio(struct bio *bio);

static inline int vdo_get_bio_result(struct bio *bio)
{
	return blk_status_to_errno(bio->bi_status);
}

static inline void vdo_complete_bio(struct bio *bio, int error)
{
	bio->bi_status = errno_to_blk_status(error);
	bio_endio(bio);
}

int vdo_create_multi_block_bio(block_count_t size, struct bio **bio_ptr);

static inline int vdo_create_bio(struct bio **bio_ptr)
{
	return vdo_create_multi_block_bio(1, bio_ptr);
}

void vdo_free_bio(struct bio *bio);

void vdo_count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio);
void vdo_count_completed_bios(struct bio *bio);

void vdo_complete_async_bio(struct bio *bio);

void vdo_set_bio_properties(struct bio *bio,
			    struct vio *vio,
			    bio_end_io_t callback,
			    unsigned int bi_opf,
			    physical_block_number_t pbn);

int vdo_reset_bio_with_buffer(struct bio *bio,
			      char *data,
			      struct vio *vio,
			      bio_end_io_t callback,
			      unsigned int bi_opf,
			      physical_block_number_t pbn);

#ifdef VDO_INTERNAL
int __must_check check_bio_validity(struct bio *bio);
#endif /* VDO_INTERNAL */
#endif /* BIO_H */
