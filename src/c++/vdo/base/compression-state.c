// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "compression-state.h"

#include <linux/atomic.h>

#include "data-vio.h"
#include "packer.h"
#include "types.h"
#include "vdo.h"
#include "vio.h"

static const uint32_t STATUS_MASK = 0xff;
static const uint32_t MAY_NOT_COMPRESS_MASK = 0x80000000;

/**
 * get_vio_compression_state() - Get the compression status of a data_vio.
 * @data_vio: The data_vio.
 *
 * Return: The compression status.
 */
struct data_vio_compression_status
get_data_vio_compression_status(struct data_vio *data_vio)
{
	uint32_t packed = atomic_read(&data_vio->compression.status);

	/* pairs with cmpxchg in set_data_vio_compression_status */
	smp_rmb();
	return (struct data_vio_compression_status) {
		.stage = packed & STATUS_MASK,
		.may_not_compress = ((packed & MAY_NOT_COMPRESS_MASK) != 0),
	};
}

/**
 * pack_status() - Convert a data_vio_compression_status into a uint32_t which
 *		   may be stored atomically.
 * @status: The state to convert.
 *
 * Return: The compression state packed into a uint32_t.
 */
static uint32_t __must_check
pack_status(struct data_vio_compression_status status)
{
	return status.stage
		| (status.may_not_compress ? MAY_NOT_COMPRESS_MASK : 0);
}

/**
 * set_data_vio_compression_status() - Set the compression status of a
 *                                     data_vio.
 * @data_vio: The data_vio whose compression status is to be set.
 * @state: The expected current status of the data_vio.
 * @new_state: The status to set.
 *
 * Return: true if the new status was set, false if the data_vio's compression
 *	   status did not match the expected state, and so was left unchanged.
 */
EXTERNAL_STATIC bool __must_check
set_data_vio_compression_status(struct data_vio *data_vio,
				struct data_vio_compression_status status,
				struct data_vio_compression_status new_status)
{
	uint32_t actual;
	uint32_t expected = pack_status(status);
	uint32_t replacement = pack_status(new_status);

	/*
	 * Extra barriers because this was original developed using a CAS
	 * operation that implicitly had them.
	 */
	smp_mb__before_atomic();
	actual = atomic_cmpxchg(&data_vio->compression.status,
				expected, replacement);
	/* same as before_atomic */
	smp_mb__after_atomic();
	return (expected == actual);
}

/**
 * advance_status() - Advance to the next compression status along the
 *		      compression path.
 * @data_vio: The data_vio to advance.
 *
 * Return: The new compression status of the data_vio.
 */
static enum data_vio_compression_stage
advance_status(struct data_vio *data_vio)
{
	for (;;) {
		struct data_vio_compression_status status =
			get_data_vio_compression_status(data_vio);
		struct data_vio_compression_status new_status = status;

		if (status.stage == DATA_VIO_POST_PACKER)
			/* We're already in the last stage. */
			return status.stage;

		if (status.may_not_compress)
			/*
			 * Compression has been dis-allowed for this VIO, so
			 * skip the rest of the path and go to the end.
			 */
			new_status.stage = DATA_VIO_POST_PACKER;
		else
			/* Go to the next state. */
			new_status.stage++;

		if (set_data_vio_compression_status(data_vio,
						    status,
						    new_status))
			return new_status.stage;

		/*
		 * Another thread changed the status out from under us so try
		 * again.
		 */
	}
}

/**
 * may_compress_data_vio() - Check whether a data_vio may go to the compressor.
 * @data_vio: The data_vio to check.
 *
 * Return: true if the data_vio may be compressed at this time.
 */
bool may_compress_data_vio(struct data_vio *data_vio)
{
	if (!data_vio_has_allocation(data_vio) ||
	    data_vio->fua ||
	    !vdo_get_compressing(vdo_from_data_vio(data_vio))) {
		/*
		 * If this VIO didn't get an allocation, the compressed write
		 * probably won't either, so don't try compressing it. Also, if
		 * compression is off, don't compress.
		 */
		set_data_vio_compression_done(data_vio);
		return false;
	}

	if (data_vio->hash_lock == NULL) {
		/*
		 * data_vios without a hash_lock (which should be extremely
		 * rare) aren't able to share the packer's PBN lock, so don't
		 * try to compress them.
		 */
		set_data_vio_compression_done(data_vio);
		return false;
	}

#ifdef __KERNEL__
	/*
	 * If the original bio was a discard, but we got this far because the
	 * discard was a partial one (r/m/w), and it is part of a larger
	 * discard, we cannot compress this vio. We need to make sure the vio
	 * completes ASAP.
	 *
	 * XXX: given the hash lock bailout, is this even possible?
	 */
	if ((data_vio->user_bio != NULL) &&
	    (bio_op(data_vio->user_bio) == REQ_OP_DISCARD) &&
	    (data_vio->remaining_discard > 0)) {
		set_data_vio_compression_done(data_vio);
		return false;
	}
#endif /* __KERNEL__ */

	return (advance_status(data_vio) == DATA_VIO_COMPRESSING);
}

/**
 * may_pack_data_vio() - Check whether a data_vio may go to the packer.
 * @data_vio: The data_vio to check.
 *
 * Return: true if the data_vio may be packed at this time.
 */
bool may_pack_data_vio(struct data_vio *data_vio)
{
	if ((data_vio->compression.size >= VDO_PACKER_BIN_SIZE) ||
	    !vdo_get_compressing(vdo_from_data_vio(data_vio)) ||
	    get_data_vio_compression_status(data_vio).may_not_compress) {
		/*
		 * If the data in this VIO doesn't compress, or compression is
		 * off, or compression for this VIO has been canceled, don't
		 * send it to the packer.
		 */
		set_data_vio_compression_done(data_vio);
		return false;
	}

	return true;
}

/**
 * may_vio_block_in_packer() - Check whether a data_vio which has gone to the
 *			       packer may block there.
 * @data_vio: The data_vio to check.
 *
 * Any cancellation after this point and before the data_vio is written out
 * requires this data_vio to be picked up by the canceling data_vio.
 *
 * Return: true if the data_vio may block in the packer.
 */
bool may_data_vio_block_in_packer(struct data_vio *data_vio)
{
	return (advance_status(data_vio) == DATA_VIO_PACKING);
}

/**
 * may_write_compressed_data_vio() - Check whether the packer may write out a
 *				     data_vio as part of a compressed block.
 * @data_vio: The data_vio to check.
 *
 * Return: true if the data_vio may be written as part of a compressed block at
 *	   this time.
 */
bool may_write_compressed_data_vio(struct data_vio *data_vio)
{
	advance_status(data_vio);
	return !get_data_vio_compression_status(data_vio).may_not_compress;
}

/**
 * set_data_vio_compression_done() - Indicate that this data_vio is leaving the
 *				     compression path.
 * @data_vio: The data_vio leaving the compression path.
 */
void set_data_vio_compression_done(struct data_vio *data_vio)
{
	for (;;) {
		struct data_vio_compression_status new_status = {
			.stage = DATA_VIO_POST_PACKER,
			.may_not_compress = true,
		};
		struct data_vio_compression_status status =
			get_data_vio_compression_status(data_vio);

		if (status.stage == DATA_VIO_POST_PACKER)
			/* The VIO is already done. */
			return;

		/*
		 * If compression was cancelled on this data_vio, preserve that
		 * fact.
		 */
		if (set_data_vio_compression_status(data_vio,
						    status,
						    new_status))
			return;
	}
}

/**
 * cancel_data_vio_compression() - Prevent this data_vio from being compressed
 *			           or packed.
 * @data_vio: The data_vio to cancel.
 *
 * Return: true if the data_vio is in the packer and the caller was the first
 *	   caller to cancel it.
 */
bool cancel_data_vio_compression(struct data_vio *data_vio)
{
	struct data_vio_compression_status status, new_status;

	for (;;) {
		status = get_data_vio_compression_status(data_vio);
		if (status.may_not_compress ||
		    (status.stage == DATA_VIO_POST_PACKER))
			/*
			 * This data_vio is already set up to not block in the
			 * packer.
			 */
			break;

		new_status.stage = status.stage;
		new_status.may_not_compress = true;

		if (set_data_vio_compression_status(data_vio,
						    status,
						    new_status))
			break;
	}

	return ((status.stage == DATA_VIO_PACKING) &&
		!status.may_not_compress);
}
