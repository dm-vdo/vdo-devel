/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMPRESSION_STATE_H
#define COMPRESSION_STATE_H

#include "types.h"

/*
 * Where a data_vio is on the compression path; advance_compression_stage() depends on the
 * order of this enum.
 */
enum data_vio_compression_stage {
	/* A data_vio which has not yet entered the compression path */
	DATA_VIO_PRE_COMPRESSOR,
	/* A data_vio which is in the compressor */
	DATA_VIO_COMPRESSING,
	/* A data_vio which is blocked in the packer */
	DATA_VIO_PACKING,
	/*
	 * A data_vio which is no longer on the compression path (and never will be)
	 */
	DATA_VIO_POST_PACKER,
};

struct data_vio_compression_status {
	enum data_vio_compression_stage stage;
	bool may_not_compress;
};

struct data_vio_compression_status __must_check
get_data_vio_compression_status(struct data_vio *data_vio);

bool __must_check may_compress_data_vio(struct data_vio *data_vio);

bool __must_check may_pack_data_vio(struct data_vio *data_vio);

bool __must_check may_data_vio_block_in_packer(struct data_vio *data_vio);

bool __must_check may_write_compressed_data_vio(struct data_vio *data_vio);

void set_data_vio_compression_done(struct data_vio *data_vio);

bool cancel_data_vio_compression(struct data_vio *data_vio);

#ifdef INTERNAL
bool set_data_vio_compression_status(struct data_vio *data_vio,
				     struct data_vio_compression_status status,
				     struct data_vio_compression_status new_status);
#endif /* INTERNAL */
#endif /* COMPRESSION_STATE_H */
