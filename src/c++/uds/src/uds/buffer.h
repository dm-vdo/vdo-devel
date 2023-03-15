/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef UDS_BUFFER_H
#define UDS_BUFFER_H

#include <linux/compiler.h>
#include <linux/types.h>

/*
 * This is an implementation of a rolling buffer for marshalling data to and from storage. The put
 * methods add data to the end of the buffer and advance the end pointer past the new data. The get
 * methods return data from the start of the buffer and advance the start pointer past anything
 * returned. Data is not actually removed until the buffer is cleared or compacted, so the same
 * data can be read multiple times if desired.
 */

struct buffer {
	size_t start;
	size_t end;
	size_t length;
	u8 *data;
	bool wrapped;
};

int __must_check uds_wrap_buffer(u8 *bytes,
				 size_t length,
				 size_t content_length,
				 struct buffer **buffer_ptr);

int __must_check uds_make_buffer(size_t length, struct buffer **buffer_ptr);
void uds_free_buffer(struct buffer *buffer);

bool __must_check uds_ensure_available_space(struct buffer *buffer, size_t bytes);

void uds_clear_buffer(struct buffer *buffer);
void uds_compact_buffer(struct buffer *buffer);
int __must_check uds_skip_forward(struct buffer *buffer, size_t bytes_to_skip);
int __must_check uds_rewind_buffer(struct buffer *buffer, size_t bytes_to_rewind);

size_t uds_buffer_length(struct buffer *buffer);
size_t uds_content_length(struct buffer *buffer);
size_t uds_available_space(struct buffer *buffer);
size_t uds_uncompacted_amount(struct buffer *buffer);
size_t uds_buffer_used(struct buffer *buffer);

int __must_check uds_reset_buffer_end(struct buffer *buffer, size_t end);

bool __must_check uds_has_same_bytes(struct buffer *buffer, const u8 *data, size_t length);
bool uds_equal_buffers(struct buffer *buffer1, struct buffer *buffer2);

int __must_check uds_get_byte(struct buffer *buffer, u8 *byte_ptr);
int __must_check uds_put_byte(struct buffer *buffer, u8 b);

int __must_check
uds_get_bytes_from_buffer(struct buffer *buffer, size_t length, void *destination);
u8 *uds_get_buffer_contents(struct buffer *buffer);
int __must_check uds_copy_bytes(struct buffer *buffer, size_t length, u8 **destination_ptr);
int __must_check uds_put_bytes(struct buffer *buffer, size_t length, const void *source);
int __must_check uds_put_buffer(struct buffer *target, struct buffer *source, size_t length);

int __must_check uds_zero_bytes(struct buffer *buffer, size_t length);

int __must_check uds_get_boolean(struct buffer *buffer, bool *b);
int __must_check uds_put_boolean(struct buffer *buffer, bool b);

int __must_check uds_get_u16_le_from_buffer(struct buffer *buffer, u16 *ui);
int __must_check uds_put_u16_le_into_buffer(struct buffer *buffer, u16 ui);

int __must_check uds_get_u16_les_from_buffer(struct buffer *buffer, size_t count, u16 *ui);
int __must_check uds_put_u16_les_into_buffer(struct buffer *buffer, size_t count, const u16 *ui);

int __must_check uds_get_s32_le_from_buffer(struct buffer *buffer, s32 *i);
int __must_check uds_get_u32_le_from_buffer(struct buffer *buffer, u32 *ui);
int __must_check uds_put_u32_le_into_buffer(struct buffer *buffer, u32 ui);

int __must_check uds_get_u64_le_from_buffer(struct buffer *buffer, u64 *ui);
int __must_check uds_put_s64_le_into_buffer(struct buffer *buffer, s64 i);
int __must_check uds_put_u64_le_into_buffer(struct buffer *buffer, u64 ui);

int __must_check uds_get_u64_les_from_buffer(struct buffer *buffer, size_t count, u64 *ui);
int __must_check uds_put_u64_les_into_buffer(struct buffer *buffer, size_t count, const u64 *ui);

#endif /* UDS_BUFFER_H */
