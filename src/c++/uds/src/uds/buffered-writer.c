// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "buffered-writer.h"

#include "compiler.h"
#ifdef TEST_INTERNAL
#include "dory.h"
#endif /* TEST_INTERNAL */
#include "errors.h"
#include "io-factory.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"

struct buffered_writer {
#ifdef __KERNEL__
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
#else
	/* Region to write to */
	struct io_region *region;
	/* Number of the current block */
	uint64_t block_number;
#endif
	/* Start of the buffer */
	byte *start;
	/* End of the data written to the buffer */
	byte *end;
	/* Error code */
	int error;
};

static INLINE size_t space_used_in_buffer(struct buffered_writer *writer)
{
	return writer->end - writer->start;
}

EXTERNAL_STATIC
size_t space_remaining_in_write_buffer(struct buffered_writer *writer)
{
	return UDS_BLOCK_SIZE - space_used_in_buffer(writer);
}

#ifdef __KERNEL__
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
#endif

#ifdef __KERNEL__
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
#else
/*
 * Make a new buffered writer.
 *
 * @param region      The IO region to write to
 * @param writer_ptr  The new buffered writer goes here
 *
 * @return UDS_SUCCESS or an error code
 */
int make_buffered_writer(struct io_region *region,
			 struct buffered_writer **writer_ptr)
{
	int result;
	byte *data;
	struct buffered_writer *writer;

	result = UDS_ALLOCATE_IO_ALIGNED(UDS_BLOCK_SIZE,
					 byte,
					 "buffered writer buffer",
					 &data);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1,
			      struct buffered_writer,
			      "buffered writer",
			      &writer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(data);
		return result;
	}

	*writer = (struct buffered_writer) {
		.region = region,
		.start = data,
		.end = data,
		.block_number = 0,
		.error = UDS_SUCCESS,
	};

	get_io_region(region);
	*writer_ptr = writer;
	return UDS_SUCCESS;
}
#endif

void free_buffered_writer(struct buffered_writer *writer)
{
	int result;

	if (writer == NULL) {
		return;
	}

#ifdef __KERNEL__
	flush_previous_buffer(writer);
	result = -dm_bufio_write_dirty_buffers(writer->client);
#else
	result = sync_region_contents(writer->region);
#endif
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "%s: failed to sync storage",
					 __func__);
	}

#ifdef __KERNEL__
	dm_bufio_client_destroy(writer->client);
	put_uds_io_factory(writer->factory);
#else
	put_io_region(writer->region);
	UDS_FREE(writer->start);
#endif
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
#ifdef __KERNEL__
		if (writer->buffer == NULL) {
			result = prepare_next_buffer(writer);
			continue;
		}

#endif
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
#ifdef __KERNEL__
		if (writer->buffer == NULL) {
			result = prepare_next_buffer(writer);
			continue;
		}
#endif

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

#ifdef __KERNEL__
	return flush_previous_buffer(writer);
#else
	if (space_used_in_buffer(writer) == 0) {
		return UDS_SUCCESS;
	}

	writer->error = write_to_region(writer->region,
					writer->block_number * UDS_BLOCK_SIZE,
					writer->start,
					UDS_BLOCK_SIZE,
					space_used_in_buffer(writer));
	if (writer->error == UDS_SUCCESS) {
		writer->end = writer->start;
		writer->block_number++;
	}

	return writer->error;
#endif
}
