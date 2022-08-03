// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "io-factory.h"

#include <linux/atomic.h>
#include <linux/blkdev.h>
#include <linux/mount.h>

#include "common.h"
#include "compiler.h"
#ifdef TEST_INTERNAL
#include "dory.h"
#endif /* TEST_INTERNAL */
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"

enum { BLK_FMODE = FMODE_READ | FMODE_WRITE };

/*
 * A kernel mode IO Factory object controls access to an index stored
 * on a block device.
 */
struct io_factory {
	struct block_device *bdev;
	atomic_t ref_count;
};

/*
 * The buffered reader allows efficient I/O for IO regions. The internal
 * buffer always reads aligned data from the underlying region.
 */
struct buffered_reader {
	/* IO factory owning the block device */
	struct io_factory *factory;
	/* The dm_bufio_client to read from */
	struct dm_bufio_client *client;
	/* The current dm_buffer */
	struct dm_buffer *buffer;
	/* The number of blocks that can be read from */
	sector_t limit;
	/* Number of the current block */
	sector_t block_number;
	/* Start of the buffer */
	byte *start;
	/* End of the data read from the buffer */
	byte *end;
};

struct buffered_writer {
	/* IO factory owning the block device */
	struct io_factory *factory;
	/* The dm_bufio_client to write to */
	struct dm_bufio_client *client;
	/* The current dm_buffer */
	struct dm_buffer *buffer;
	/* The number of blocks that can be written to */
	sector_t limit;
	/* Number of the current block */
	sector_t block_number;
	/* Start of the buffer */
	byte *start;
	/* End of the data written to the buffer */
	byte *end;
	/* Error code */
	int error;
};

void get_uds_io_factory(struct io_factory *factory)
{
	atomic_inc(&factory->ref_count);
}

static int get_block_device_from_name(const char *name,
				      struct block_device **bdev)
{
	dev_t device = name_to_dev_t(name);

	if (device != 0) {
		*bdev = blkdev_get_by_dev(device, BLK_FMODE, NULL);
	} else {
		*bdev = blkdev_get_by_path(name, BLK_FMODE, NULL);
	}
	if (IS_ERR(*bdev)) {
		uds_log_error_strerror(-PTR_ERR(*bdev),
				       "%s is not a block device", name);
		return UDS_INVALID_ARGUMENT;
	}

	return UDS_SUCCESS;
}

int make_uds_io_factory(const char *path, struct io_factory **factory_ptr)
{
	int result;
	struct block_device *bdev;
	struct io_factory *factory;

	result = get_block_device_from_name(path, &bdev);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct io_factory, __func__, &factory);
	if (result != UDS_SUCCESS) {
		blkdev_put(bdev, BLK_FMODE);
		return result;
	}

	factory->bdev = bdev;
	atomic_set_release(&factory->ref_count, 1);

	*factory_ptr = factory;
	return UDS_SUCCESS;
}

int replace_uds_storage(struct io_factory *factory, const char *path)
{
	int result;
	struct block_device *bdev;

	result = get_block_device_from_name(path, &bdev);
	if (result != UDS_SUCCESS) {
		return result;
	}

	blkdev_put(factory->bdev, BLK_FMODE);
	factory->bdev = bdev;
	return UDS_SUCCESS;
}

void put_uds_io_factory(struct io_factory *factory)
{
	if (atomic_add_return(-1, &factory->ref_count) <= 0) {
		blkdev_put(factory->bdev, BLK_FMODE);
		UDS_FREE(factory);
	}
}

size_t get_uds_writable_size(struct io_factory *factory)
{
	return i_size_read(factory->bdev->bd_inode);
}

int make_uds_bufio(struct io_factory *factory,
		   off_t offset,
		   size_t block_size,
		   unsigned int reserved_buffers,
		   struct dm_bufio_client **client_ptr)
{
	struct dm_bufio_client *client;

	if (offset % SECTOR_SIZE != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "offset %zd not multiple of %d",
					      offset,
					      SECTOR_SIZE);
	}
	if (block_size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"block_size %zd not multiple of %d",
			block_size,
			UDS_BLOCK_SIZE);
	}

	client = dm_bufio_client_create(
		factory->bdev, block_size, reserved_buffers, 0, NULL, NULL);
	if (IS_ERR(client)) {
		return -PTR_ERR(client);
	}

	dm_bufio_set_sector_offset(client, offset >> SECTOR_SHIFT);
	*client_ptr = client;
	return UDS_SUCCESS;
}

static void read_ahead(struct buffered_reader *reader, sector_t block_number)
{
	if (block_number < reader->limit) {
		enum { MAX_READ_AHEAD = 4 };
		sector_t read_ahead = min((sector_t) MAX_READ_AHEAD,
					  reader->limit - block_number);

		dm_bufio_prefetch(reader->client, block_number, read_ahead);
	}
}

/*
 * Make a new buffered reader.
 *
 * @param factory      The IO factory creating the buffered reader
 * @param client       The dm_bufio_client to read from
 * @param block_limit  The number of blocks that may be read
 * @param reader_ptr   The pointer to hold the newly allocated buffered reader
 *
 * @return UDS_SUCCESS or error code
 */
int make_buffered_reader(struct io_factory *factory,
			 struct dm_bufio_client *client,
			 sector_t block_limit,
			 struct buffered_reader **reader_ptr)
{
	int result;
	struct buffered_reader *reader = NULL;

	result = UDS_ALLOCATE(1,
			      struct buffered_reader,
			      "buffered reader",
			      &reader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*reader = (struct buffered_reader) {
		.factory = factory,
		.client = client,
		.buffer = NULL,
		.limit = block_limit,
		.block_number = 0,
		.start = NULL,
		.end = NULL,
	};

	read_ahead(reader, 0);
	get_uds_io_factory(factory);
	*reader_ptr = reader;
	return UDS_SUCCESS;
}

void free_buffered_reader(struct buffered_reader *reader)
{
	if (reader == NULL) {
		return;
	}

	if (reader->buffer != NULL) {
		dm_bufio_release(reader->buffer);
	}

	dm_bufio_client_destroy(reader->client);
	put_uds_io_factory(reader->factory);
	UDS_FREE(reader);
}

int open_uds_buffered_reader(struct io_factory *factory,
			     off_t offset,
			     size_t size,
			     struct buffered_reader **reader_ptr)
{
	int result;
	struct dm_bufio_client *client = NULL;

	if (size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(
			UDS_INCORRECT_ALIGNMENT,
			"region size %zd is not multiple of %d",
			size,
			UDS_BLOCK_SIZE);
	}

	result = make_uds_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffered_reader(
		factory, client, size / UDS_BLOCK_SIZE, reader_ptr);
	if (result != UDS_SUCCESS) {
		dm_bufio_client_destroy(client);
	}
	return result;
}

static int position_reader(struct buffered_reader *reader,
			   sector_t block_number,
			   off_t offset)
{
	if ((reader->end == NULL) || (block_number != reader->block_number)) {
		struct dm_buffer *buffer = NULL;
		void *data;

		if (block_number >= reader->limit) {
			return UDS_OUT_OF_RANGE;
		}

		if (reader->buffer != NULL) {
			dm_bufio_release(reader->buffer);
			reader->buffer = NULL;
		}

		data = dm_bufio_read(reader->client, block_number, &buffer);
		if (IS_ERR(data)) {
			return -PTR_ERR(data);
		}

		reader->buffer = buffer;
		reader->start = data;
		if (block_number == reader->block_number + 1) {
			read_ahead(reader, block_number + 1);
		}
	}

	reader->block_number = block_number;
	reader->end = reader->start + offset;
	return UDS_SUCCESS;
}

static size_t bytes_remaining_in_read_buffer(struct buffered_reader *reader)
{
	return (reader->end == NULL ?
		0 :
		reader->start + UDS_BLOCK_SIZE - reader->end);
}

static int reset_reader(struct buffered_reader *reader)
{
	sector_t block_number;

	if (bytes_remaining_in_read_buffer(reader) > 0) {
		return UDS_SUCCESS;
	}

	block_number = reader->block_number;
	if (reader->end != NULL) {
		++block_number;
	}

	return position_reader(reader, block_number, 0);
}

/*
 * Retrieve data from a buffered reader, reading from the region when needed.
 *
 * @param reader  The buffered reader
 * @param data    The buffer to read data into
 * @param length  The length of the data to read
 *
 * @return UDS_SUCCESS or an error code
 */
int read_from_buffered_reader(struct buffered_reader *reader,
			      void *data,
			      size_t length)
{
	byte *dp = data;
	int result = UDS_SUCCESS;
	size_t chunk;

	while (length > 0) {
		result = reset_reader(reader);
		if (result != UDS_SUCCESS) {
			break;
		}

		chunk = min(length, bytes_remaining_in_read_buffer(reader));
		memcpy(dp, reader->end, chunk);
		length -= chunk;
		dp += chunk;
		reader->end += chunk;
	}

	return result;
}

/*
 * Verify that the data currently in the buffer matches the required value.
 *
 * @param reader  The buffered reader
 * @param value   The value that must match the buffer contents
 * @param length  The length of the value that must match
 *
 * @return UDS_SUCCESS or UDS_CORRUPT_DATA if the value does not match
 *
 * @note If the value matches, the matching contents are consumed. However,
 *       if the match fails, any buffer contents are left as is.
 */
int verify_buffered_data(struct buffered_reader *reader,
			 const void *value,
			 size_t length)
{
	int result = UDS_SUCCESS;
	size_t chunk;
	const byte *vp = value;
	sector_t start_block_number = reader->block_number;
	int start_offset = reader->end - reader->start;

	while (length > 0) {
		result = reset_reader(reader);
		if (result != UDS_SUCCESS) {
			result = UDS_CORRUPT_DATA;
			break;
		}

		chunk = min(length, bytes_remaining_in_read_buffer(reader));
		if (memcmp(vp, reader->end, chunk) != 0) {
			result = UDS_CORRUPT_DATA;
			break;
		}

		length -= chunk;
		vp += chunk;
		reader->end += chunk;
	}

	if (result != UDS_SUCCESS) {
		position_reader(reader, start_block_number, start_offset);
	}

	return result;
}

/*
 * Make a new buffered writer.
 *
 * @param factory      The IO factory creating the buffered writer
 * @param client       The dm_bufio_client to write to
 * @param block_limit  The number of blocks that may be written to
 * @param writer_ptr   The new buffered writer goes here
 *
 * @return UDS_SUCCESS or an error code
 */
int make_buffered_writer(struct io_factory *factory,
			 struct dm_bufio_client *client,
			 sector_t block_limit,
			 struct buffered_writer **writer_ptr)
{
	int result;
	struct buffered_writer *writer;

	result = UDS_ALLOCATE(1,
			      struct buffered_writer,
			      "buffered writer",
			      &writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*writer = (struct buffered_writer) {
		.factory = factory,
		.client = client,
		.buffer = NULL,
		.limit = block_limit,
		.start = NULL,
		.end = NULL,
		.block_number = 0,
		.error = UDS_SUCCESS,
	};

	get_uds_io_factory(factory);
	*writer_ptr = writer;
	return UDS_SUCCESS;
}

int open_uds_buffered_writer(struct io_factory *factory,
			     off_t offset,
			     size_t size,
			     struct buffered_writer **writer_ptr)
{
	int result;
	struct dm_bufio_client *client = NULL;

	if (size % UDS_BLOCK_SIZE != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "region size %zd is not multiple of %d",
					      size,
					      UDS_BLOCK_SIZE);
	}

	result = make_uds_bufio(factory, offset, UDS_BLOCK_SIZE, 1, &client);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffered_writer(
		factory, client, size / UDS_BLOCK_SIZE, writer_ptr);
	if (result != UDS_SUCCESS) {
		dm_bufio_client_destroy(client);
	}
	return result;
}

static INLINE size_t space_used_in_buffer(struct buffered_writer *writer)
{
	return writer->end - writer->start;
}

EXTERNAL_STATIC
size_t space_remaining_in_write_buffer(struct buffered_writer *writer)
{
	return UDS_BLOCK_SIZE - space_used_in_buffer(writer);
}

static int __must_check prepare_next_buffer(struct buffered_writer *writer)
{
	struct dm_buffer *buffer = NULL;
	void *data;

	if (writer->block_number >= writer->limit) {
		writer->error = UDS_OUT_OF_RANGE;
		return UDS_OUT_OF_RANGE;
	}

	data = dm_bufio_new(writer->client, writer->block_number, &buffer);
	if (IS_ERR(data)) {
		writer->error = -PTR_ERR(data);
		return writer->error;
	}

	writer->buffer = buffer;
	writer->start = data;
	writer->end = data;
	return UDS_SUCCESS;
}

static int flush_previous_buffer(struct buffered_writer *writer)
{
	size_t available;

	if (writer->buffer == NULL) {
		return writer->error;
	}

	if (writer->error == UDS_SUCCESS) {
		available = space_remaining_in_write_buffer(writer);

		if (available > 0) {
			memset(writer->end, 0, available);
		}

#ifdef TEST_INTERNAL
		if (get_dory_forgetful()) {
			writer->error = -EROFS;
		} else {
			dm_bufio_mark_buffer_dirty(writer->buffer);
		}
#else
		dm_bufio_mark_buffer_dirty(writer->buffer);
#endif /* TEST_INTERNAL */
	}

	dm_bufio_release(writer->buffer);
	writer->buffer = NULL;
	writer->start = NULL;
	writer->end = NULL;
	writer->block_number++;
	return writer->error;
}

void free_buffered_writer(struct buffered_writer *writer)
{
	int result;

	if (writer == NULL) {
		return;
	}

	flush_previous_buffer(writer);
	result = -dm_bufio_write_dirty_buffers(writer->client);
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "%s: failed to sync storage",
					 __func__);
	}

	dm_bufio_client_destroy(writer->client);
	put_uds_io_factory(writer->factory);
	UDS_FREE(writer);
}

/*
 * Append data to the buffer, writing as needed. If a write error occurs, it
 * is recorded and returned on every subsequent write attempt.
 */
int write_to_buffered_writer(struct buffered_writer *writer,
			     const void *data,
			     size_t len)
{
	const byte *dp = data;
	int result = UDS_SUCCESS;
	size_t chunk;

	if (writer->error != UDS_SUCCESS) {
		return writer->error;
	}

	while ((len > 0) && (result == UDS_SUCCESS)) {
		if (writer->buffer == NULL) {
			result = prepare_next_buffer(writer);
			continue;
		}

		chunk = min(len, space_remaining_in_write_buffer(writer));
		memcpy(writer->end, dp, chunk);
		len -= chunk;
		dp += chunk;
		writer->end += chunk;

		if (space_remaining_in_write_buffer(writer) == 0) {
			result = flush_buffered_writer(writer);
		}
	}

	return result;
}

int write_zeros_to_buffered_writer(struct buffered_writer *writer, size_t len)
{
	int result = UDS_SUCCESS;
	size_t chunk;

	if (writer->error != UDS_SUCCESS) {
		return writer->error;
	}

	while ((len > 0) && (result == UDS_SUCCESS)) {
		if (writer->buffer == NULL) {
			result = prepare_next_buffer(writer);
			continue;
		}

		chunk = min(len, space_remaining_in_write_buffer(writer));
		memset(writer->end, 0, chunk);
		len -= chunk;
		writer->end += chunk;

		if (space_remaining_in_write_buffer(writer) == 0) {
			result = flush_buffered_writer(writer);
		}
	}

	return result;
}

int flush_buffered_writer(struct buffered_writer *writer)
{
	if (writer->error != UDS_SUCCESS) {
		return writer->error;
	}

	return flush_previous_buffer(writer);
}
