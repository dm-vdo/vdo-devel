/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef IO_FACTORY_H
#define IO_FACTORY_H

#include <linux/dm-bufio.h>

#include "type-defs.h"

/*
 * The I/O factory manages all low-level I/O operations to the underlying storage device. Its main
 * clients are the index layout and the volume. The buffered reader and buffered writer interfaces
 * are helpers for accessing data in a contiguous range of storage blocks.
 */

struct buffered_reader;
struct buffered_writer;

struct io_factory;

enum {
	UDS_BLOCK_SIZE = 4096,
	SECTORS_PER_BLOCK = UDS_BLOCK_SIZE >> SECTOR_SHIFT,
};

int __must_check make_uds_io_factory(const char *path, struct io_factory **factory_ptr);

int __must_check replace_uds_storage(struct io_factory *factory, const char *path);

void put_uds_io_factory(struct io_factory *factory);

size_t __must_check get_uds_writable_size(struct io_factory *factory);

int __must_check make_uds_bufio(struct io_factory *factory,
				off_t block_offset,
				size_t block_size,
				unsigned int reserved_buffers,
				struct dm_bufio_client **client_ptr);

int __must_check make_buffered_reader(struct io_factory *factory,
				      off_t offset,
				      u64 block_count,
				      struct buffered_reader **reader_ptr);

void free_buffered_reader(struct buffered_reader *reader);

int __must_check
read_from_buffered_reader(struct buffered_reader *reader, byte *data, size_t length);

int __must_check
verify_buffered_data(struct buffered_reader *reader, const byte *value, size_t length);

int __must_check make_buffered_writer(struct io_factory *factory,
				      off_t offset,
				      u64 block_count,
				      struct buffered_writer **writer_ptr);

void free_buffered_writer(struct buffered_writer *buffer);

int __must_check
write_to_buffered_writer(struct buffered_writer *writer, const byte *data, size_t length);

int __must_check flush_buffered_writer(struct buffered_writer *writer);

#endif /* IO_FACTORY_H */
