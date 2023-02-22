/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef DELTAINDEX_H
#define DELTAINDEX_H 1

#include <linux/cache.h>

#include "config.h"
#include "io-factory.h"
#include "numeric.h"
#include "time-utils.h"

/*
 * A delta index is a key-value store, where each entry maps an address (the key) to a payload (the
 * value). The entries are sorted by address, and only the delta between successive addresses is
 * stored in the entry. The addresses are assumed to be uniformly distributed, and the deltas are
 * therefore exponentially distributed.
 *
 * A delta_index can either be mutable or immutable depending on its expected use. The immutable
 * form of a delta index is used for the indexes of closed chapters committed to the volume. The
 * mutable form of a delta index is used by the volume index, and also by the chapter index in an
 * open chapter. Like the index as a whole, each mutable delta index is divided into a number of
 * independent zones.
 */

struct delta_list {
	/* The offset of the delta list start, in bits */
	u64 start;
	/* The number of bits in the delta list */
	u16 size;
	/* Where the last search "found" the key, in bits */
	u16 save_offset;
	/* The key for the record just before save_offset */
	unsigned int save_key;
};

struct delta_zone {
	/* The delta list memory */
	u8 *memory;
	/* The delta list headers */
	struct delta_list *delta_lists;
	/* Temporary starts of delta lists */
	u64 *new_offsets;
	/* Buffered writer for saving an index */
	struct buffered_writer *buffered_writer;
	/* The size of delta list memory */
	size_t size;
	/* Nanoseconds spent rebalancing */
	ktime_t rebalance_time;
	/* Number of memory rebalances */
	int rebalance_count;
	/* The number of bits in a stored value */
	unsigned short value_bits;
	/* The number of bits in the minimal key code */
	unsigned short min_bits;
	/* The number of keys used in a minimal code */
	unsigned int min_keys;
	/* The number of keys used for another code bit */
	unsigned int incr_keys;
	/* The number of records in the index */
	long record_count;
	/* The number of collision records */
	long collision_count;
	/* The number of records removed */
	long discard_count;
	/* The number of UDS_OVERFLOW errors detected */
	long overflow_count;
	/* The index of the first delta list */
	unsigned int first_list;
	/* The number of delta lists */
	unsigned int list_count;
	/* Tag belonging to this delta index */
	u8 tag;
} __aligned(L1_CACHE_BYTES);

struct delta_list_save_info {
	/* Tag identifying which delta index this list is in */
	u8 tag;
	/* Bit offset of the start of the list data */
	u8 bit_offset;
	/* Number of bytes of list data */
	u16 byte_count;
	/* The delta list number within the delta index */
	u32 index;
};

struct delta_index {
	/* The zones */
	struct delta_zone *delta_zones;
	/* The number of zones */
	unsigned int zone_count;
	/* The number of delta lists */
	unsigned int list_count;
	/* Maximum lists per zone */
	unsigned int lists_per_zone;
	/* The number of non-empty lists at load time per zone */
	unsigned int load_lists[MAX_ZONES];
	/* True if this index is mutable */
	bool mutable;
	/* Tag belonging to this delta index */
	u8 tag;
};

/*
 * A delta_index_page describes a single page of a chapter index. The delta_index field allows the
 * page to be treated as an immutable delta_index. We use the delta_zone field to treat the chapter
 * index page as a single zone index, and without the need to do an additional memory allocation.
 */
struct delta_index_page {
	struct delta_index delta_index;
	/* These values are loaded from the DeltaPageHeader */
	unsigned int lowest_list_number;
	unsigned int highest_list_number;
	u64 virtual_chapter_number;
	/* This structure describes the single zone of a delta index page. */
	struct delta_zone delta_zone;
};

/*
 * Notes on the delta_index_entries:
 *
 * The fields documented as "public" can be read by any code that uses a delta_index. The fields
 * documented as "private" carry information between delta_index method calls and should not be
 * used outside the delta_index module.
 *
 * (1) The delta_index_entry is used like an iterator when searching a delta list.
 *
 * (2) It is also the result of a successful search and can be used to refer to the element found
 *     by the search.
 *
 * (3) It is also the result of an unsuccessful search and can be used to refer to the insertion
 *     point for a new record.
 *
 * (4) If at_end is true, the delta_list entry can only be used as the insertion point for a new
 *     record at the end of the list.
 *
 * (5) If at_end is false and is_collision is true, the delta_list entry fields refer to a
 *     collision entry in the list, and the delta_list entry can be used a a reference to this
 *     entry.
 *
 * (6) If at_end is false and is_collision is false, the delta_list entry fields refer to a
 *     non-collision entry in the list. Such delta_list entries can be used as a reference to a
 *     found entry, or an insertion point for a non-collision entry before this entry, or an
 *     insertion point for a collision entry that collides with this entry.
 */
struct delta_index_entry {
	/* Public fields */
	/* The key for this entry */
	unsigned int key;
	/* We are after the last list entry */
	bool at_end;
	/* This record is a collision */
	bool is_collision;

	/* Private fields */
	/* This delta list overflowed */
	bool list_overflow;
	/* The number of bits used for the value */
	unsigned short value_bits;
	/* The number of bits used for the entire entry */
	unsigned short entry_bits;
	/* The delta index zone */
	struct delta_zone *delta_zone;
	/* The delta list containing the entry */
	struct delta_list *delta_list;
	/* The delta list number */
	unsigned int list_number;
	/* Bit offset of this entry within the list */
	u32 offset;
	/* The delta between this and previous entry */
	unsigned int delta;
	/* Temporary delta list for immutable indices */
	struct delta_list temp_delta_list;
};

struct delta_index_stats {
	/* Number of bytes allocated */
	size_t memory_allocated;
	/* Nanoseconds spent rebalancing */
	ktime_t rebalance_time;
	/* Number of memory rebalances */
	int rebalance_count;
	/* The number of records in the index */
	long record_count;
	/* The number of collision records */
	long collision_count;
	/* The number of records removed */
	long discard_count;
	/* The number of UDS_OVERFLOW errors detected */
	long overflow_count;
	/* The number of delta lists */
	unsigned int list_count;
};

#ifdef TEST_INTERNAL
void move_bits(const u8 *from, u64 from_offset, u8 *to, u64 to_offset, int size);

int __must_check extend_delta_zone(struct delta_zone *delta_zone,
				   unsigned int growing_index,
				   size_t growing_size);

void swap_delta_index_page_endianness(u8 *memory);

void uninitialize_delta_zone(struct delta_zone *delta_zone);

int __must_check initialize_delta_zone(struct delta_zone *delta_zone,
				       size_t size,
				       unsigned int first_list,
				       unsigned int list_count,
				       unsigned int mean_delta,
				       unsigned int payload_bits);

#endif /* TEST_INTERNAL */
int __must_check initialize_delta_index(struct delta_index *delta_index,
					unsigned int zone_count,
					unsigned int list_count,
					unsigned int mean_delta,
					unsigned int payload_bits,
					size_t memory_size);

int __must_check initialize_delta_index_page(struct delta_index_page *delta_index_page,
					     u64 expected_nonce,
					     unsigned int mean_delta,
					     unsigned int payload_bits,
					     u8 *memory,
					     size_t memory_size);

void uninitialize_delta_index(struct delta_index *delta_index);

void empty_delta_index(const struct delta_index *delta_index);

int __must_check pack_delta_index_page(const struct delta_index *delta_index,
				       u64 header_nonce,
				       u8 *memory,
				       size_t memory_size,
				       u64 virtual_chapter_number,
				       unsigned int first_list,
				       unsigned int *list_count);

void set_delta_index_tag(struct delta_index *delta_index, u8 tag);

int __must_check start_restoring_delta_index(struct delta_index *delta_index,
					     struct buffered_reader **buffered_readers,
					     unsigned int reader_count);

int __must_check finish_restoring_delta_index(struct delta_index *delta_index,
					      struct buffered_reader **buffered_readers,
					      unsigned int reader_count);

void abort_restoring_delta_index(const struct delta_index *delta_index);

int __must_check
check_guard_delta_lists(struct buffered_reader **buffered_readers, unsigned int reader_count);

int __must_check start_saving_delta_index(const struct delta_index *delta_index,
					  unsigned int zone_number,
					  struct buffered_writer *buffered_writer);

int __must_check
finish_saving_delta_index(const struct delta_index *delta_index, unsigned int zone_number);

int __must_check
write_guard_delta_list(struct buffered_writer *buffered_writer);

size_t __must_check compute_delta_index_save_bytes(unsigned int list_count, size_t memory_size);

int __must_check start_delta_index_search(const struct delta_index *delta_index,
					  unsigned int list_number,
					  unsigned int key,
					  struct delta_index_entry *iterator);

int __must_check next_delta_index_entry(struct delta_index_entry *delta_entry);

int __must_check
remember_delta_index_offset(const struct delta_index_entry *delta_entry);

int __must_check get_delta_index_entry(const struct delta_index *delta_index,
				       unsigned int list_number,
				       unsigned int key,
				       const u8 *name,
				       struct delta_index_entry *delta_entry);

int __must_check
get_delta_entry_collision(const struct delta_index_entry *delta_entry, u8 *name);

unsigned int __must_check get_delta_entry_value(const struct delta_index_entry *delta_entry);

int __must_check
set_delta_entry_value(const struct delta_index_entry *delta_entry, unsigned int value);

int __must_check put_delta_index_entry(struct delta_index_entry *delta_entry,
				       unsigned int key,
				       unsigned int value,
				       const u8 *name);

int __must_check
remove_delta_index_entry(struct delta_index_entry *delta_entry);

static inline unsigned int
get_delta_zone_number(const struct delta_index *delta_index, unsigned int list_number)
{
	return list_number / delta_index->lists_per_zone;
}

unsigned int
get_delta_zone_first_list(const struct delta_index *delta_index, unsigned int zone_number);

unsigned int
get_delta_zone_list_count(const struct delta_index *delta_index, unsigned int zone_number);

u64 __must_check
get_delta_zone_bits_used(const struct delta_index *delta_index, unsigned int zone_number);

u64 __must_check get_delta_index_bits_allocated(const struct delta_index *delta_index);

void get_delta_index_stats(const struct delta_index *delta_index, struct delta_index_stats *stats);

size_t __must_check compute_delta_index_size(unsigned long entry_count,
					     unsigned int mean_delta,
					     unsigned int payload_bits);

unsigned int get_delta_index_page_count(unsigned int entry_count,
					unsigned int list_count,
					unsigned int mean_delta,
					unsigned int payload_bits,
					size_t bytes_per_page);

void log_delta_index_entry(struct delta_index_entry *delta_entry);

#endif /* DELTAINDEX_H */
