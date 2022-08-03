/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef IO_FACTORY_H
#define IO_FACTORY_H

#include <linux/dm-bufio.h>

struct buffered_reader;
struct buffered_writer;

struct io_factory;

enum { UDS_BLOCK_SIZE = 4096 };

int __must_check make_uds_io_factory(const char *path,
				     struct io_factory **factory_ptr);

int __must_check replace_uds_storage(struct io_factory *factory,
				     const char *path);

void get_uds_io_factory(struct io_factory *factory);

void put_uds_io_factory(struct io_factory *factory);

size_t __must_check get_uds_writable_size(struct io_factory *factory);

int __must_check make_uds_bufio(struct io_factory *factory,
				off_t offset,
				size_t block_size,
				unsigned int reserved_buffers,
				struct dm_bufio_client **client_ptr);

int __must_check open_uds_buffered_reader(struct io_factory *factory,
					  off_t offset,
					  size_t size,
					  struct buffered_reader **reader_ptr);

int __must_check make_buffered_reader(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      struct buffered_reader **reader_ptr);

void free_buffered_reader(struct buffered_reader *reader);

int __must_check read_from_buffered_reader(struct buffered_reader *reader,
					   void *data,
					   size_t length);

int __must_check verify_buffered_data(struct buffered_reader *reader,
				      const void *value,
				      size_t length);

int __must_check open_uds_buffered_writer(struct io_factory *factory,
					  off_t offset,
					  size_t size,
					  struct buffered_writer **writer_ptr);

int __must_check make_buffered_writer(struct io_factory *factory,
				      struct dm_bufio_client *client,
				      sector_t block_limit,
				      struct buffered_writer **writer_ptr);

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
#endif /* IO_FACTORY_H */
