/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BUFFERED_WRITER_H
#define BUFFERED_WRITER_H 1

#include "common.h"

struct buffered_writer;
#ifdef __KERNEL__
struct dm_bufio_client;
struct io_factory;
#else
struct io_region;
#endif

#ifdef __KERNEL__
int __must_check make_buffered_writer(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      struct buffered_writer **writer_ptr);
#else
int __must_check make_buffered_writer(struct io_region *region,
				      struct buffered_writer **writer_ptr);
#endif

void free_buffered_writer(struct buffered_writer *buffer);

int __must_check write_to_buffered_writer(struct buffered_writer *writer,
					  const void *data,
					  size_t len);

int __must_check write_zeros_to_buffered_writer(struct buffered_writer *writer,
						size_t len);

int __must_check flush_buffered_writer(struct buffered_writer *writer);

#ifdef TEST_INTERNAL
size_t __must_check
space_remaining_in_write_buffer(struct buffered_writer *writer);

#endif /* TEST_INTERNAL */
#endif /* BUFFERED_WRITER_H */
