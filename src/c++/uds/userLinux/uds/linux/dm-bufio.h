/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef LINUX_DM_BUFIO_H
#define LINUX_DM_BUFIO_H

#include <linux/blkdev.h>

/* These are just the parts of dm-bufio interface that UDS uses. */

struct dm_bufio_client;
struct dm_buffer;

/*
 * Flags for dm_bufio_client_create
 */
#define DM_BUFIO_CLIENT_NO_SLEEP 0x1

struct dm_bufio_client *
dm_bufio_client_create(struct block_device *bdev,
		       unsigned block_size,
		       unsigned reserved_buffers,
		       unsigned aux_size,
		       void (*alloc_callback)(struct dm_buffer *),
		       void (*write_callback)(struct dm_buffer *),
		       unsigned int flags);

void dm_bufio_client_destroy(struct dm_bufio_client *client);

void dm_bufio_set_sector_offset(struct dm_bufio_client *client,
				sector_t start);

void *dm_bufio_new(struct dm_bufio_client *client,
		   sector_t block,
		   struct dm_buffer **buffer_ptr);

void *dm_bufio_read(struct dm_bufio_client *client,
		    sector_t block,
		    struct dm_buffer **buffer_ptr);

void dm_bufio_prefetch(struct dm_bufio_client *client,
		       sector_t block,
		       unsigned block_count);

void dm_bufio_release(struct dm_buffer *buffer);

void dm_bufio_release_move(struct dm_buffer *buffer, sector_t new_block);

void dm_bufio_mark_buffer_dirty(struct dm_buffer *buffer);

int dm_bufio_write_dirty_buffers(struct dm_bufio_client *client);

void *dm_bufio_get_block_data(struct dm_buffer *buffer);

#endif /* LINUX_DM_BUFIO_H */
