// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "buffer.h"

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"
#include "string-utils.h"

/*
 * Create a buffer which wraps an existing byte array.
 *
 * @bytes: The bytes to wrap
 * @length: The length of the buffer
 * @content_length: The length of the current contents of the buffer
 * @buffer_ptr: A pointer to hold the buffer
 *
 * Return: UDS_SUCCESS or an error code
 */
int uds_wrap_buffer(u8 *bytes, size_t length, size_t content_length, struct buffer **buffer_ptr)
{
	int result;
	struct buffer *buffer;

	result = ASSERT((content_length <= length),
			"content length, %zu, fits in buffer size, %zu",
			length,
			content_length);

	result = UDS_ALLOCATE(1, struct buffer, "buffer", &buffer);
	if (result != UDS_SUCCESS)
		return result;

	buffer->data = bytes;
	buffer->start = 0;
	buffer->end = content_length;
	buffer->length = length;
	buffer->wrapped = true;

	*buffer_ptr = buffer;
	return UDS_SUCCESS;
}

/*
 * Create a new buffer and allocate its memory.
 *
 * @size: The length of the buffer
 * @new_buffer: A pointer to hold the buffer
 *
 * Return: UDS_SUCCESS or an error code
 */
int make_uds_buffer(size_t size, struct buffer **new_buffer)
{
	int result;
	u8 *data;
	struct buffer *buffer;

	result = UDS_ALLOCATE(size, u8, "buffer data", &data);
	if (result != UDS_SUCCESS)
		return result;

	result = uds_wrap_buffer(data, size, 0, &buffer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(UDS_FORGET(data));
		return result;
	}

	buffer->wrapped = false;
	*new_buffer = buffer;
	return UDS_SUCCESS;
}

void free_uds_buffer(struct buffer *buffer)
{
	if (buffer == NULL)
		return;

	if (!buffer->wrapped)
		UDS_FREE(UDS_FORGET(buffer->data));

	UDS_FREE(buffer);
}

size_t uds_buffer_length(struct buffer *buffer)
{
	return buffer->length;
}

/* Return the amount of data currently in the buffer. */
size_t uds_content_length(struct buffer *buffer)
{
	return buffer->end - buffer->start;
}

/* Return the amount of data that has already been processed. */
size_t uds_uncompacted_amount(struct buffer *buffer)
{
	return buffer->start;
}

/* Return the amount of space available in the buffer. */
size_t uds_available_space(struct buffer *buffer)
{
	return buffer->length - buffer->end;
}

/* Return the amount of the buffer that is currently utilized. */
size_t uds_buffer_used(struct buffer *buffer)
{
	return buffer->end;
}

/*
 * Ensure that a buffer has a given amount of space available, compacting the buffer if necessary.
 * Returns true if the space is available.
 */
bool uds_ensure_available_space(struct buffer *buffer, size_t bytes)
{
	if (uds_available_space(buffer) >= bytes)
		return true;

	uds_compact_buffer(buffer);
	return uds_available_space(buffer) >= bytes;
}

void uds_clear_buffer(struct buffer *buffer)
{
	buffer->start = 0;
	buffer->end = buffer->length;
}

/*
 * Eliminate buffer contents which have been extracted. This function copies any data between the
 * start and end pointers to the beginning of the buffer, moves the start pointer to the beginning,
 * and the end pointer to the end of the copied data.
 */
void uds_compact_buffer(struct buffer *buffer)
{
	size_t bytes_to_move;

	if ((buffer->start == 0) || (buffer->end == 0))
		return;

	bytes_to_move = buffer->end - buffer->start;
	memmove(buffer->data, buffer->data + buffer->start, bytes_to_move);
	buffer->start = 0;
	buffer->end = bytes_to_move;
}

/* Reset the end of buffer to a different position. */
int uds_reset_buffer_end(struct buffer *buffer, size_t end)
{
	if (end > buffer->length)
		return UDS_BUFFER_ERROR;

	buffer->end = end;
	if (buffer->start > buffer->end)
		buffer->start = buffer->end;

	return UDS_SUCCESS;
}

/* Advance the start pointer by the specified number of bytes. */
int uds_skip_forward(struct buffer *buffer, size_t bytes_to_skip)
{
	if (uds_content_length(buffer) < bytes_to_skip)
		return UDS_BUFFER_ERROR;

	buffer->start += bytes_to_skip;
	return UDS_SUCCESS;
}

/* Rewind the start pointer by the specified number of bytes. */
int uds_rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind)
{
	if (buffer->start < bytes_to_rewind)
		return UDS_BUFFER_ERROR;

	buffer->start -= bytes_to_rewind;
	return UDS_SUCCESS;
}

/* Check whether the start of the contents of a buffer matches a specified array of bytes. */
bool uds_has_same_bytes(struct buffer *buffer, const u8 *data, size_t length)
{
	return (uds_content_length(buffer) >= length) &&
	       (memcmp(buffer->data + buffer->start, data, length) == 0);
}

/* Check whether two buffers have the same contents. */
bool uds_equal_buffers(struct buffer *buffer1, struct buffer *buffer2)
{
	return uds_has_same_bytes(buffer1,
				  buffer2->data + buffer2->start,
				  uds_content_length(buffer2));
}

int uds_get_byte(struct buffer *buffer, u8 *byte_ptr)
{
	if (uds_content_length(buffer) < sizeof(u8))
		return UDS_BUFFER_ERROR;

	*byte_ptr = buffer->data[buffer->start++];
	return UDS_SUCCESS;
}

int uds_put_byte(struct buffer *buffer, u8 b)
{
	if (!uds_ensure_available_space(buffer, sizeof(u8)))
		return UDS_BUFFER_ERROR;

	buffer->data[buffer->end++] = b;
	return UDS_SUCCESS;
}

int uds_get_bytes_from_buffer(struct buffer *buffer, size_t length, void *destination)
{
	if (uds_content_length(buffer) < length)
		return UDS_BUFFER_ERROR;

	memcpy(destination, buffer->data + buffer->start, length);
	buffer->start += length;
	return UDS_SUCCESS;
}

/*
 * Get a pointer to the current contents of the buffer. This will be a pointer to the actual memory
 * managed by the buffer. It is the caller's responsibility to ensure that the buffer is not
 * modified while this pointer is in use.
 */
u8 *uds_get_buffer_contents(struct buffer *buffer)
{
	return buffer->data + buffer->start;
}

/*
 * Copy bytes out of a buffer as per uds_get_bytes_from_buffer(). Memory will be allocated to hold
 * the copy.
 */
int uds_copy_bytes(struct buffer *buffer, size_t length, u8 **destination_ptr)
{
	int result;
	u8 *destination;

	result = UDS_ALLOCATE(length, u8, __func__, &destination);
	if (result != UDS_SUCCESS)
		return result;

	result = uds_get_bytes_from_buffer(buffer, length, destination);
	if (result != UDS_SUCCESS)
		UDS_FREE(destination);
	else
		*destination_ptr = destination;

	return result;
}

int uds_put_bytes(struct buffer *buffer, size_t length, const void *source)
{
	if (!uds_ensure_available_space(buffer, length))
		return UDS_BUFFER_ERROR;

	memcpy(buffer->data + buffer->end, source, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

/*
 * Copy the contents of a source buffer into the target buffer. This is equivalent to calling
 * uds_get_byte() on the source and uds_put_byte() on the target repeatedly.
 */
int uds_put_buffer(struct buffer *target, struct buffer *source, size_t length)
{
	int result;

	if (uds_content_length(source) < length)
		return UDS_BUFFER_ERROR;

	result = uds_put_bytes(target, length, uds_get_buffer_contents(source));
	if (result != UDS_SUCCESS)
		return result;

	source->start += length;
	return UDS_SUCCESS;
}

/* Put the specified number of zero bytes in the buffer. */
int uds_zero_bytes(struct buffer *buffer, size_t length)
{
	if (!uds_ensure_available_space(buffer, length))
		return UDS_BUFFER_ERROR;

	memset(buffer->data + buffer->end, 0, length);
	buffer->end += length;
	return UDS_SUCCESS;
}

int uds_get_boolean(struct buffer *buffer, bool *b)
{
	int result;
	u8 value;

	result = uds_get_byte(buffer, &value);
	if (result == UDS_SUCCESS)
		*b = (value == 1);

	return result;
}

int uds_put_boolean(struct buffer *buffer, bool b)
{
	return uds_put_byte(buffer, (u8) (b ? 1 : 0));
}

int uds_get_u16_le_from_buffer(struct buffer *buffer, u16 *ui)
{
	if (uds_content_length(buffer) < sizeof(u16))
		return UDS_BUFFER_ERROR;

	decode_u16_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int uds_put_u16_le_into_buffer(struct buffer *buffer, u16 ui)
{
	if (!uds_ensure_available_space(buffer, sizeof(u16)))
		return UDS_BUFFER_ERROR;

	encode_u16_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int uds_get_u16_les_from_buffer(struct buffer *buffer, size_t count, u16 *ui)
{
	unsigned int i;

	if (uds_content_length(buffer) < (sizeof(u16) * count))
		return UDS_BUFFER_ERROR;

	for (i = 0; i < count; i++)
		decode_u16_le(buffer->data, &buffer->start, ui + i);

	return UDS_SUCCESS;
}

int uds_put_u16_les_into_buffer(struct buffer *buffer, size_t count, const u16 *ui)
{
	unsigned int i;

	if (!uds_ensure_available_space(buffer, sizeof(u16) * count))
		return UDS_BUFFER_ERROR;

	for (i = 0; i < count; i++)
		encode_u16_le(buffer->data, &buffer->end, ui[i]);

	return UDS_SUCCESS;
}

int uds_get_s32_le_from_buffer(struct buffer *buffer, s32 *i)
{
	if (uds_content_length(buffer) < sizeof(s32))
		return UDS_BUFFER_ERROR;

	decode_s32_le(buffer->data, &buffer->start, i);
	return UDS_SUCCESS;
}

int uds_get_u32_le_from_buffer(struct buffer *buffer, u32 *ui)
{
	if (uds_content_length(buffer) < sizeof(u32))
		return UDS_BUFFER_ERROR;

	decode_u32_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int uds_put_u32_le_into_buffer(struct buffer *buffer, u32 ui)
{
	if (!uds_ensure_available_space(buffer, sizeof(u32)))
		return UDS_BUFFER_ERROR;

	encode_u32_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int uds_put_s64_le_into_buffer(struct buffer *buffer, s64 i)
{
	if (!uds_ensure_available_space(buffer, sizeof(s64)))
		return UDS_BUFFER_ERROR;

	encode_s64_le(buffer->data, &buffer->end, i);
	return UDS_SUCCESS;
}

int uds_get_u64_le_from_buffer(struct buffer *buffer, u64 *ui)
{
	if (uds_content_length(buffer) < sizeof(u64))
		return UDS_BUFFER_ERROR;

	decode_u64_le(buffer->data, &buffer->start, ui);
	return UDS_SUCCESS;
}

int uds_put_u64_le_into_buffer(struct buffer *buffer, u64 ui)
{
	if (!uds_ensure_available_space(buffer, sizeof(u64)))
		return UDS_BUFFER_ERROR;

	encode_u64_le(buffer->data, &buffer->end, ui);
	return UDS_SUCCESS;
}

int uds_get_u64_les_from_buffer(struct buffer *buffer, size_t count, u64 *ui)
{
	unsigned int i;

	if (uds_content_length(buffer) < (sizeof(u64) * count))
		return UDS_BUFFER_ERROR;

	for (i = 0; i < count; i++)
		decode_u64_le(buffer->data, &buffer->start, ui + i);

	return UDS_SUCCESS;
}

int uds_put_u64_les_into_buffer(struct buffer *buffer, size_t count, const u64 *ui)
{
	unsigned int i;

	if (!uds_ensure_available_space(buffer, sizeof(u64) * count))
		return UDS_BUFFER_ERROR;

	for (i = 0; i < count; i++)
		encode_u64_le(buffer->data, &buffer->end, ui[i]);

	return UDS_SUCCESS;
}
