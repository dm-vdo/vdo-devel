// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "delta-index.h"

#include "buffer.h"
#include "compiler.h"
#include "config.h"
#include "cpu.h"
#include "errors.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "string-utils.h"
#include "time-utils.h"
#include "type-defs.h"
#include "uds.h"

/*
 * A delta index is a key-value store, where each entry maps an address (the
 * key) to a payload (the value). The entries are sorted by address, and only
 * the delta between successive addresses is stored in the entry. The
 * addresses are assumed to be uniformly distributed, and the deltas are
 * therefore exponentially distributed.
 *
 * The entries could be stored in a single delta list, but for efficiency we
 * use multiple delta lists. These lists are stored in a single chunk of memory
 * managed by the delta_memory module. The delta_memory module can move the
 * data around in memory, so we never keep any byte pointers into delta_list
 * memory. We only keep offsets into the memory.
 *
 * The delta lists are stored as bit streams. These bit streams are stored in
 * little endian order, and all offsets into delta_memory are bit offsets.
 *
 * All entries are stored as a fixed length payload (the value) followed by a
 * variable length key (the delta), and always strictly in little endian
 * order.
 *
 * A collision entry is used when two block names have the same delta list
 * address. A collision entry is encoded with DELTA == 0, and has 256
 * extension bits containing the full block name.
 *
 * The DELTA == 0 encoding usually indicates a collision with the preceding
 * entry, but for the first entry in any delta list there is no preceding
 * entry, so the DELTA == 0 encoding at the beginning of a delta list
 * indicates a normal entry.
 *
 * The Huffman code is driven by 3 parameters:
 *
 *  MINBITS   This is the number of bits in the smallest code
 *
 *  BASE      This is the number of values coded using a code of length MINBITS
 *
 *  INCR      This is the number of values coded by using one additional bit
 *
 * These parameters are related by:
 *
 *      BASE + INCR == 1 << MINBITS
 *
 * When we create an index, we need to know the mean delta. From the mean
 * delta, we compute these three parameters. The math for the Huffman code of
 * an exponential distribution says that we compute
 *
 *      INCR = log(2) * MEAN_DELTA
 *
 * Then we find the smallest MINBITS so that
 *
 *      (1 << MINBITS) > INCR
 *
 * And then
 *
 *      BASE = (1 << MINBITS) - INCR
 *
 * Now we need a code such that
 *
 * - The first BASE values code using MINBITS bits.
 * - The next INCR values code using MINBITS+1 bits.
 * - The next INCR values code using MINBITS+2 bits.
 * - The next INCR values code using MINBITS+3 bits.
 * - (and so on).
 *
 * ENCODE(DELTA):
 *
 *   if (DELTA < BASE) {
 *       put DELTA in MINBITS bits;
 *   } else {
 *       T1 = (DELTA - BASE) % INCR + BASE;
 *       T2 = (DELTA - BASE) / INCR;
 *       put T1 in MINBITS bits;
 *       put 0 in T2 bits;
 *       put 1 in 1 bit;
 *   }
 *
 * DECODE(BIT_STREAM):
 *
 *   T1 = next MINBITS bits of stream;
 *   if (T1 < BASE) {
 *       DELTA = T1;
 *   } else {
 *       Scan bits in the stream until reading a 1,
 *         setting T2 to the number of 0 bits read;
 *       DELTA = T2 * INCR + T1;
 *   }
 *
 * The bit field utilities that we use on the delta lists assume that it is
 * possible to read a few bytes beyond the end of the bit field, so we make
 * sure to allocate some extra bytes at the end of memory containing the delta
 * lists. Consult the bit utilities documentation for more details.
 *
 * Note that the decode bit stream code includes a step that skips over 0 bits
 * until the first 1 bit is found. A corrupted delta list could cause this
 * step to run off the end of the delta list memory. As an extra protection
 * against this happening, the guard bytes at the end should be set to all
 * ones.
 */

/*
 * The delta_memory structure manages the memory that stores delta lists.
 * Because the volume index can contain a million delta lists or more, we
 * want to be efficient with the delta list header size.
 *
 * The delta list information is encoded into 16 bytes per list. The volume
 * index delta list memory can easily exceed 4 gigabits, so we must use a
 * uint64_t to address the memory. The volume index delta lists average around
 * 6 kilobits, so we can represent the size of a delta list with a uint16_t.
 *
 * The delta memory contains N delta lists, which are guarded by two
 * empty delta lists. The valid delta lists are numbered 1 to N, and the
 * guard lists are numbered 0 and N+1.
 *
 * The delta_memory supports two different forms. The mutable form is created
 * by initialize_delta_memory(), and is used for the volume index and for open
 * chapter indexes. The immutable form is created by
 * initialize_delta_memory_page(), and is used for cached chapter index
 * pages. The immutable form does not allocate delta list headers or temporary
 * offsets, and thus is somewhat more memory efficient.
 */

/*
 * These bit stream and bit field utility routines are used for the delta
 * indexes, which are not byte-aligned.
 *
 * Bits and bytes are numbered in little endian order. Within a byte, bit 0
 * is the least significant bit (0x1), and bit 7 is the most significant bit
 * (0x80). Within a bit stream, bit 7 is the most signficant bit of byte 0,
 * and bit 8 is the least significant bit of byte 1. Within a byte array, a
 * byte's number corresponds to its index in the array.
 *
 * This implementation assumes that the native machine is little endian, and
 * that performance is very important.
 */

/* This is the number of bits in a uint32_t. */
enum {
	UINT32_BITS = sizeof(uint32_t) * CHAR_BIT,
};

/*
 * This is the largest field size supported by get_field() and set_field().
 * Any field that is larger is not guaranteed to fit in a single byte-aligned
 * uint32_t.
 */
enum {
	MAX_FIELD_BITS = (sizeof(uint32_t) - 1) * CHAR_BIT + 1,
};

/*
 * This is the largest field size supported by get_big_field() and
 * set_big_field(). Any field that is larger is not guaranteed to fit in a
 * single byte-aligned uint64_t.
 */
enum {
	MAX_BIG_FIELD_BITS = (sizeof(uint64_t) - 1) * CHAR_BIT + 1,
};

/*
 * This is the number of guard bytes needed at the end of the memory byte
 * array when using the bit utilities. 3 bytes are needed when get_field() and
 * set_field() access a field, because they will access some extra bytes past
 * the end of the field. 7 bytes are needed when get_big_field() and
 * set_big_field() access a big field, for the same reason. Note that
 * move_bits() calls get_big_field() and set_big_field(). The definition is
 * written to make it clear how it is derived.
 */
enum {
	POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1,
};

/* The number of guard bits that are needed in the tail guard list */
enum {
	GUARD_BITS = POST_FIELD_GUARD_BYTES * CHAR_BIT
};

/*
 * The maximum size of a single delta list (in bytes). We count guard bytes
 * in this value because a buffer of this size can be used with move_bits().
 */
enum {
	DELTA_LIST_MAX_BYTE_COUNT =
		((UINT16_MAX + CHAR_BIT) / CHAR_BIT + POST_FIELD_GUARD_BYTES)
};

/* The number of extra bytes and bits needed to store a collision entry */
enum {
	COLLISION_BYTES = UDS_CHUNK_NAME_SIZE,
	COLLISION_BITS = COLLISION_BYTES * CHAR_BIT
};

/*
 * Immutable delta lists are packed into pages containing a header that
 * encodes the delta list information into 19 bits per list (64KB bit offset).
 */

enum { IMMUTABLE_HEADER_SIZE = 19 };

/*
 * Constants and structures for the saved delta index. "DI" is for
 * delta_index, and -##### is a number to increment when the format of the
 * data changes.
 */

enum {
	MAGIC_SIZE = 8,
};

static const char MAGIC_DI_START[] = "DI-00002";

struct di_header {
	char magic[MAGIC_SIZE];
	uint32_t zone_number;
	uint32_t num_zones;
	uint32_t first_list;
	uint32_t num_lists;
	uint64_t record_count;
	uint64_t collision_count;
};

/*
 * Header data used for immutable delta index pages. This data is followed by
 * the delta list offset table.
 */
struct delta_page_header {
	/* Externally-defined nonce */
	uint64_t nonce;
	/* The virtual chapter number */
	uint64_t virtual_chapter_number;
	/* Index of the first delta list on the page */
	uint16_t first_list;
	/* Number of delta lists on the page */
	uint16_t num_lists;
} __packed;


#ifndef TEST_INTERNAL
static INLINE uint64_t
get_delta_list_start(const struct delta_list *delta_list)
{
	return delta_list->start_offset;
}

static INLINE uint16_t
get_delta_list_size(const struct delta_list *delta_list)
{
	return delta_list->size;
}

static INLINE uint64_t get_delta_list_end(const struct delta_list *delta_list)
{
	return delta_list->start_offset + delta_list->size;
}

#endif /* TEST_INTERNAL */
static INLINE uint64_t
get_delta_list_byte_start(const struct delta_list *delta_list)
{
	return get_delta_list_start(delta_list) / CHAR_BIT;
}

static INLINE uint16_t
get_delta_list_byte_size(const struct delta_list *delta_list)
{
	unsigned int bit_offset = get_delta_list_start(delta_list) % CHAR_BIT;
	unsigned int bit_size = get_delta_list_size(delta_list);

	return DIV_ROUND_UP(bit_offset + bit_size, CHAR_BIT);
}

static void rebalance_delta_memory(const struct delta_memory *delta_memory,
				   unsigned int first,
				   unsigned int last)
{
	struct delta_list *delta_list;
	uint64_t new_start;

	if (first == last) {
		/* Only one list is moving, and we know there is space. */
		delta_list = &delta_memory->delta_lists[first];
		new_start = delta_memory->temp_offsets[first];
		if (get_delta_list_start(delta_list) != new_start) {
			uint64_t destination, source;
			source = get_delta_list_byte_start(delta_list);
			delta_list->start_offset = new_start;
			destination = get_delta_list_byte_start(delta_list);
			memmove(delta_memory->memory + destination,
				delta_memory->memory + source,
				get_delta_list_byte_size(delta_list));
		}
	} else {
		/*
		 * There is more than one list. Divide the problem in half,
		 * and use recursive calls to process each half. Note that
		 * after this computation, first <= middle, and middle < last.
		 */
		unsigned int middle = (first + last) / 2;

		delta_list = &delta_memory->delta_lists[middle];
		new_start = delta_memory->temp_offsets[middle];

		/*
		 * The direction that our middle list is moving determines
		 * which half of the problem must be processed first.
		 */
		if (new_start > get_delta_list_start(delta_list)) {
			rebalance_delta_memory(delta_memory, middle + 1, last);
			rebalance_delta_memory(delta_memory, first, middle);
		} else {
			rebalance_delta_memory(delta_memory, first, middle);
			rebalance_delta_memory(delta_memory, middle + 1, last);
		}
	}
}

/* Move the start of the delta list bit stream without moving the end. */
static INLINE void move_delta_list_start(struct delta_list *delta_list,
					 int increment)
{
	delta_list->start_offset += increment;
	delta_list->size -= increment;
}

/* Move the end of the delta list bit stream without moving the start. */
static INLINE void move_delta_list_end(struct delta_list *delta_list,
				       int increment)
{
	delta_list->size += increment;
}

static INLINE size_t get_zone_memory_size(unsigned int num_zones,
					  size_t memory_size)
{
	size_t zone_size = memory_size / num_zones;

	/* Round up so that each zone is a multiple of 64K in size. */
	enum {
		ALLOC_BOUNDARY = 64 * KILOBYTE,
	};

	return (zone_size + ALLOC_BOUNDARY - 1) & -ALLOC_BOUNDARY;
}

static void empty_delta_lists(struct delta_memory *delta_memory)
{
	uint64_t list_bits;
	uint64_t spacing;
	uint64_t offset;
	unsigned int i;
	struct delta_list *delta_lists = delta_memory->delta_lists;

	/*
	 * Initialize delta lists to be empty. We keep 2 extra delta list
	 * descriptors, one before the first real entry and one after so that
	 * we don't need to bounds check the array access when calculating
	 * preceeding and following gap sizes.
	 *
	 * Because the delta list headers are zeroed, the head guard list will
	 * already be at offset zero and size zero.
	 *
	 * The end guard list contains guard bytes so that get_field() and
	 * get_big_field() can safely read past the end of any byte we are
	 * interested in.
	 */

	/* Zero all the delta list headers. */
	memset(delta_lists,
	       0,
	       (delta_memory->num_lists + 2) * sizeof(struct delta_list));

	/* Set all the bits in the end guard list. */
	list_bits = (uint64_t) delta_memory->size * CHAR_BIT - GUARD_BITS;
	delta_lists[delta_memory->num_lists + 1].start_offset = list_bits;
	delta_lists[delta_memory->num_lists + 1].size = GUARD_BITS;
	memset(delta_memory->memory + (list_bits / CHAR_BIT),
	       ~0,
	       POST_FIELD_GUARD_BYTES);

	/* Evenly space out the real delta lists by setting regular offsets. */
	spacing = list_bits / delta_memory->num_lists;
	offset = spacing / 2;
	for (i = 1; i <= delta_memory->num_lists; i++) {
		delta_lists[i].start_offset = offset;
		offset += spacing;
	}

	/* Update the statistics. */
	delta_memory->discard_count += delta_memory->record_count;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
}

void empty_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;

	for (z = 0; z < delta_index->num_zones; z++) {
		empty_delta_lists(&delta_index->delta_zones[z]);
	}
}

void empty_delta_index_zone(const struct delta_index *delta_index,
			    unsigned int zone_number)
{
	empty_delta_lists(&delta_index->delta_zones[zone_number]);
}

/* Compute the Huffman coding parameters for the given mean delta. */
static void compute_coding_constants(unsigned int mean_delta,
				     unsigned short *min_bits,
				     unsigned int *min_keys,
				     unsigned int *incr_keys)
{
	/*
	 * We want to compute the rounded value of log(2) * mean_delta. Since
	 * we cannot always use floating point, use a really good integer
	 * approximation.
	 */
	*incr_keys = (836158UL * mean_delta + 603160UL) / 1206321UL;
	*min_bits = compute_bits(*incr_keys + 1);
	*min_keys = (1 << *min_bits) - *incr_keys;
}

EXTERNAL_STATIC void uninitialize_delta_memory(struct delta_memory *delta_memory)
{
	UDS_FREE(delta_memory->temp_offsets);
	delta_memory->temp_offsets = NULL;
	UDS_FREE(delta_memory->delta_lists);
	delta_memory->delta_lists = NULL;
	UDS_FREE(delta_memory->memory);
	delta_memory->memory = NULL;
}

void uninitialize_delta_index(struct delta_index *delta_index)
{
	unsigned int z;

	if (delta_index->delta_zones == NULL) {
		return;
	}

	for (z = 0; z < delta_index->num_zones; z++) {
		uninitialize_delta_memory(&delta_index->delta_zones[z]);
	}

	UDS_FREE(delta_index->delta_zones);
	memset(delta_index, 0, sizeof(struct delta_index));
}

EXTERNAL_STATIC int initialize_delta_memory(struct delta_memory *delta_memory,
					    size_t size,
					    unsigned int first_list,
					    unsigned int num_lists,
					    unsigned int mean_delta,
					    unsigned int num_payload_bits)
{
	int result;
	result = UDS_ALLOCATE(size, byte, "delta list", &delta_memory->memory);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(num_lists + 2,
			      uint64_t,
			      "delta list temp",
			      &delta_memory->temp_offsets);
	if (result != UDS_SUCCESS) {
		uninitialize_delta_memory(delta_memory);
		return result;
	}

	/* Allocate the delta lists. */
	result = UDS_ALLOCATE(num_lists + 2,
			      struct delta_list,
			      "delta lists",
			      &delta_memory->delta_lists);
	if (result != UDS_SUCCESS) {
		uninitialize_delta_memory(delta_memory);
		return result;
	}

	compute_coding_constants(mean_delta,
				 &delta_memory->min_bits,
				 &delta_memory->min_keys,
				 &delta_memory->incr_keys);
	delta_memory->value_bits = num_payload_bits;
	delta_memory->buffered_writer = NULL;
	delta_memory->size = size;
	delta_memory->rebalance_time = 0;
	delta_memory->rebalance_count = 0;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
	delta_memory->discard_count = 0;
	delta_memory->overflow_count = 0;
	delta_memory->first_list = first_list;
	delta_memory->num_lists = num_lists;
	delta_memory->tag = 'm';

	empty_delta_lists(delta_memory);
	return UDS_SUCCESS;
}

int initialize_delta_index(struct delta_index *delta_index,
			   unsigned int num_zones,
			   unsigned int num_lists,
			   unsigned int mean_delta,
			   unsigned int num_payload_bits,
			   size_t memory_size)
{
	int result;
	unsigned int z;
	size_t zone_memory;

	result = UDS_ALLOCATE(num_zones,
			      struct delta_memory,
			      "Delta Index Zones",
			      &delta_index->delta_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_index->num_zones = num_zones;
	delta_index->num_lists = num_lists;
	delta_index->lists_per_zone = DIV_ROUND_UP(num_lists, num_zones);
	delta_index->is_mutable = true;
	delta_index->tag = 'm';

	for (z = 0; z < num_zones; z++) {
		unsigned int num_lists_in_zone = delta_index->lists_per_zone;
		unsigned int first_list_in_zone = z * num_lists_in_zone;

		if (z == num_zones - 1) {
			/*
			 * The last zone gets fewer lists if num_zones doesn't
			 * evenly divide num_lists. We'll have an underflow if
			 * the assertion below doesn't hold.
			 */
			if (delta_index->num_lists <= first_list_in_zone) {
				uninitialize_delta_index(delta_index);
				return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
							      "%u delta lists not enough for %u zones",
							      num_lists,
							      num_zones);
			}
			num_lists_in_zone =
				delta_index->num_lists - first_list_in_zone;
		}

		zone_memory = get_zone_memory_size(num_zones, memory_size);
		result = initialize_delta_memory(&delta_index->delta_zones[z],
						 zone_memory,
						 first_list_in_zone,
						 num_lists_in_zone,
						 mean_delta,
						 num_payload_bits);
		if (result != UDS_SUCCESS) {
			uninitialize_delta_index(delta_index);
			return result;
		}
	}

	return UDS_SUCCESS;
}

/* Read a bit field from an arbitrary bit boundary. */
static INLINE unsigned int
get_field(const byte *memory, uint64_t offset, int size)
{
	const void *addr = memory + offset / CHAR_BIT;

	return ((get_unaligned_le32(addr) >> (offset % CHAR_BIT)) &
		((1 << size) - 1));
}

/* Write a bit field to an arbitrary bit boundary. */
static INLINE void
set_field(unsigned int value, byte *memory, uint64_t offset, int size)
{
	void *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint32_t data = get_unaligned_le32(addr);

	data &= ~(((1 << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le32(data, addr);
}

/* Get the bit offset to the immutable delta list header. */
static INLINE unsigned int get_immutable_header_offset(unsigned int list_number)
{
	return (sizeof(struct delta_page_header) * CHAR_BIT +
		list_number * IMMUTABLE_HEADER_SIZE);
}

/* Get the bit offset to the start of the immutable delta list bit stream. */
static INLINE unsigned int get_immutable_start(const byte *memory,
					       unsigned int list_number)
{
	return get_field(memory,
			 get_immutable_header_offset(list_number),
			 IMMUTABLE_HEADER_SIZE);
}

/* Set the bit offset to the start of the immutable delta list bit stream. */
static INLINE void set_immutable_start(byte *memory,
				       unsigned int list_number,
				       unsigned int start_offset)
{
	set_field(start_offset,
		  memory,
		  get_immutable_header_offset(list_number),
		  IMMUTABLE_HEADER_SIZE);
}

static bool verify_delta_index_page(uint64_t nonce,
				    uint16_t list_count,
				    uint64_t expected_nonce,
				    byte *memory,
				    size_t memory_size)
{
	unsigned int i;

	/*
	 * Verify the nonce. A mismatch can happen here during rebuild if we
	 * haven't written the entire volume at least once.
	 */
	if (nonce != expected_nonce) {
		return false;
	}

	/* Verify that the number of delta lists can fit in the page. */
	if (list_count > ((memory_size - sizeof(struct delta_page_header)) *
			  CHAR_BIT / IMMUTABLE_HEADER_SIZE)) {
		return false;
	}

	/*
	 * Verify that the first delta list is immediately after the last delta
	 * list header.
	 */
	if (get_immutable_start(memory, 0) !=
	    get_immutable_header_offset(list_count + 1)) {
		return false;
	}

	/* Verify that the lists are in the correct order. */
	for (i = 0; i < list_count; i++) {
		if (get_immutable_start(memory, i) >
		    get_immutable_start(memory, i + 1)) {
			return false;
		}
	}

	/*
	 * Verify that the last list ends on the page, and that there is room
	 * for the post-field guard bits.
	 */
	if (get_immutable_start(memory, list_count) >
	    (memory_size - POST_FIELD_GUARD_BYTES) * CHAR_BIT) {
		return false;
	}

	/* Verify that the guard bytes are correctly set to all ones. */
	for (i = 0; i < POST_FIELD_GUARD_BYTES; i++) {
		byte guard_byte;

		guard_byte = memory[memory_size - POST_FIELD_GUARD_BYTES + i];
		if (guard_byte != (byte) ~0) {
			return false;
		}
	}

	/* All verifications passed. */
	return true;
}

EXTERNAL_STATIC void
initialize_delta_memory_page(struct delta_memory *delta_memory,
			     byte *memory,
			     size_t size,
			     unsigned int num_lists,
			     unsigned int mean_delta,
			     unsigned int num_payload_bits)
{
	compute_coding_constants(mean_delta,
				 &delta_memory->min_bits,
				 &delta_memory->min_keys,
				 &delta_memory->incr_keys);
	delta_memory->value_bits = num_payload_bits;
	delta_memory->memory = memory;
	delta_memory->delta_lists = NULL;
	delta_memory->temp_offsets = NULL;
	delta_memory->buffered_writer = NULL;
	delta_memory->size = size;
	delta_memory->rebalance_time = 0;
	delta_memory->rebalance_count = 0;
	delta_memory->record_count = 0;
	delta_memory->collision_count = 0;
	delta_memory->discard_count = 0;
	delta_memory->overflow_count = 0;
	delta_memory->first_list = 0;
	delta_memory->num_lists = num_lists;
	delta_memory->tag = 'p';
}

/* Initialize a delta index page to refer to a supplied page. */
int initialize_delta_index_page(struct delta_index_page *delta_index_page,
				uint64_t expected_nonce,
				unsigned int mean_delta,
				unsigned int num_payload_bits,
				byte *memory,
				size_t memory_size)
{
	uint64_t nonce;
	uint64_t vcn;
	uint64_t first_list;
	uint64_t num_lists;
	struct delta_page_header *header = (struct delta_page_header *) memory;
        const byte *nonce_addr = (const byte *) &header->nonce;
        const byte *vcn_addr = (const byte *) &header->virtual_chapter_number;
        const byte *first_list_addr = (const byte *) &header->first_list;
        const byte *num_lists_addr = (const byte *) &header->num_lists;

	/* First assume that the header is little endian. */
	nonce = get_unaligned_le64(nonce_addr);
	vcn = get_unaligned_le64(vcn_addr);
	first_list = get_unaligned_le16(first_list_addr);
	num_lists = get_unaligned_le16(num_lists_addr);
	if (!verify_delta_index_page(nonce,
				     num_lists,
				     expected_nonce,
				     memory,
				     memory_size)) {
		/* If that fails, try big endian. */
		nonce = get_unaligned_be64(nonce_addr);
		vcn = get_unaligned_be64(vcn_addr);
		first_list = get_unaligned_be16(first_list_addr);
		num_lists = get_unaligned_be16(num_lists_addr);
		if (!verify_delta_index_page(nonce,
					     num_lists,
					     expected_nonce,
					     memory,
					     memory_size)) {
			/*
			 * Both attempts failed. Do not log this as an error,
			 * because it can happen during a rebuild if we haven't
			 * written the entire volume at least once.
			 */
			return UDS_CORRUPT_DATA;
		}
	}

	delta_index_page->delta_index.delta_zones =
		&delta_index_page->delta_memory;
	delta_index_page->delta_index.num_zones = 1;
	delta_index_page->delta_index.num_lists = num_lists;
	delta_index_page->delta_index.lists_per_zone = num_lists;
	delta_index_page->delta_index.is_mutable = false;
	delta_index_page->delta_index.tag = 'p';
	delta_index_page->virtual_chapter_number = vcn;
	delta_index_page->lowest_list_number = first_list;
	delta_index_page->highest_list_number = first_list + num_lists - 1;

	initialize_delta_memory_page(&delta_index_page->delta_memory,
				     memory,
				     memory_size,
				     num_lists,
				     mean_delta,
				     num_payload_bits);
	return UDS_SUCCESS;
}

/* Read a large bit field from an arbitrary bit boundary. */
static INLINE uint64_t get_big_field(const byte *memory,
				     uint64_t offset,
				     int size)
{
	const void *addr = memory + offset / CHAR_BIT;

	return ((get_unaligned_le64(addr) >> (offset % CHAR_BIT)) &
		((1UL << size) - 1));
}

/* Write a large bit field to an arbitrary bit boundary. */
static INLINE void
set_big_field(uint64_t value, byte *memory, uint64_t offset, int size)
{
	void *addr = memory + offset / CHAR_BIT;
	int shift = offset % CHAR_BIT;
	uint64_t data = get_unaligned_le64(addr);

	data &= ~(((1UL << size) - 1) << shift);
	data |= value << shift;
	put_unaligned_le64(data, addr);
}

/* Set a sequence of bits to all zeros. */
static INLINE void set_zero(byte *memory, uint64_t offset, int size)
{
	if (size > 0) {
		byte *addr = memory + offset / CHAR_BIT;
		int shift = offset % CHAR_BIT;
		int count = size + shift > CHAR_BIT ? CHAR_BIT - shift : size;

		*addr++ &= ~(((1 << count) - 1) << shift);
		for (size -= count; size > CHAR_BIT; size -= CHAR_BIT) {
			*addr++ = 0;
		}

		if (size > 0) {
			*addr &= 0xFF << size;
		}
	}
}

/*
 * Move several bits from a higher to a lower address, moving the lower
 * addressed bits first. The size and memory offsets are measured in bits.
 */
static void move_bits_down(const byte *s_memory,
			   uint64_t source,
			   byte *d_memory,
			   uint64_t destination,
			   int size)
{
	const byte *src;
	byte *dest;
	int offset;
	int count;
	uint64_t field;

	/* Start by moving one field that ends on a destination int boundary. */
	count = (MAX_BIG_FIELD_BITS -
		 ((destination + MAX_BIG_FIELD_BITS) % UINT32_BITS));
	field = get_big_field(s_memory, source, count);
	set_big_field(field, d_memory, destination, count);
	source += count;
	destination += count;
	size -= count;

	/*
	 * Now do the main loop to copy 32 bit chunks that are int-aligned at
	 * the destination.
	 */
	offset = source % UINT32_BITS;
	src = s_memory + (source - offset) / CHAR_BIT;
	dest = d_memory + destination / CHAR_BIT;
	while (size > MAX_BIG_FIELD_BITS) {
		put_unaligned_le32(get_unaligned_le64(src) >> offset, dest);
		src += sizeof(uint32_t);
		dest += sizeof(uint32_t);
		source += UINT32_BITS;
		destination += UINT32_BITS;
		size -= UINT32_BITS;
	}

	/* Finish up by moving any remaining bits. */
	if (size > 0) {
		field = get_big_field(s_memory, source, size);
		set_big_field(field, d_memory, destination, size);
	}
}

/*
 * Move several bits from a lower to a higher address, moving the higher
 * addressed bits first. The size and memory offsets are measured in bits.
 */
static void move_bits_up(const byte *s_memory,
			 uint64_t source,
			 byte *d_memory,
			 uint64_t destination,
			 int size)
{
	const byte *src;
	byte *dest;
	int offset;
	int count;
	uint64_t field;

	/* Start by moving one field that begins on a destination int boundary. */
	count = (destination + size) % UINT32_BITS;
	if (count > 0) {
		size -= count;
		field = get_big_field(s_memory, source + size, count);
		set_big_field(field, d_memory, destination + size, count);
	}

	/*
	 * Now do the main loop to copy 32 bit chunks that are int-aligned at
	 * the destination.
	 */
	offset = (source + size) % UINT32_BITS;
	src = s_memory + (source + size - offset) / CHAR_BIT;
	dest = d_memory + (destination + size) / CHAR_BIT;
	while (size > MAX_BIG_FIELD_BITS) {
		src -= sizeof(uint32_t);
		dest -= sizeof(uint32_t);
		size -= UINT32_BITS;
		put_unaligned_le32(get_unaligned_le64(src) >> offset, dest);
	}

	/* Finish up by moving any remaining bits. */
	if (size > 0) {
		field = get_big_field(s_memory, source, size);
		set_big_field(field, d_memory, destination, size);
	}
}

/*
 * Move bits from one field to another. When the fields overlap, behave as if
 * we first move all the bits from the source to a temporary value, and then
 * move all the bits from the temporary value to the destination. The size and
 * memory offsets are measured in bits.
 */
EXTERNAL_STATIC void move_bits(const byte *s_memory,
			       uint64_t source,
			       byte *d_memory,
			       uint64_t destination,
			       int size)
{
	uint64_t field;

	/* A small move doesn't require special handling. */
	if (size <= MAX_BIG_FIELD_BITS) {
		if (size > 0) {
			field = get_big_field(s_memory, source, size);
			set_big_field(field, d_memory, destination, size);
		}

		return;
	}

	if (source > destination) {
		move_bits_down(s_memory, source, d_memory, destination, size);
	} else {
		move_bits_up(s_memory, source, d_memory, destination, size);
	}
}

/**
 * Pack delta lists from a mutable delta index into an immutable delta index
 * page. A range of delta lists (starting with a specified list index) is
 * copied from the mutable delta index into a memory page used in the immutable
 * index. The number of lists copied onto the page is returned in num_lists.
 **/
int pack_delta_index_page(const struct delta_index *delta_index,
			  uint64_t header_nonce,
			  byte *memory,
			  size_t memory_size,
			  uint64_t virtual_chapter_number,
			  unsigned int first_list,
			  unsigned int *num_lists)
{
	const struct delta_memory *delta_zone;
	struct delta_list *delta_lists;
	unsigned int max_lists;
	unsigned int n_lists = 0;
	unsigned int offset;
	unsigned int i;
	int free_bits;
	int bits;
	struct delta_page_header *header;

	delta_zone = &delta_index->delta_zones[0];
	delta_lists = &delta_zone->delta_lists[first_list + 1];
	max_lists = delta_index->num_lists - first_list;

	/*
	 * Compute how many lists will fit on the page. Subtract the size of
	 * the fixed header, one delta list offset, and the guard bytes from
	 * the page size to determine how much space is available for delta
	 * lists.
	 */
	free_bits = memory_size * CHAR_BIT;
	free_bits -= get_immutable_header_offset(1);
	free_bits -= POST_FIELD_GUARD_BYTES * CHAR_BIT;
	if (free_bits < IMMUTABLE_HEADER_SIZE) {
		/* This page is too small to store any delta lists. */
		return uds_log_error_strerror(UDS_OVERFLOW,
					      "Chapter Index Page of %zu bytes is too small",
					      memory_size);
	}

	while (n_lists < max_lists) {
		/* Each list requires a delta list offset and the list data. */
		bits = (IMMUTABLE_HEADER_SIZE +
		        get_delta_list_size(&delta_lists[n_lists]));
		if (bits > free_bits) {
			break;
		}

		n_lists++;
		free_bits -= bits;
	}

	*num_lists = n_lists;

	header = (struct delta_page_header *) memory;
	put_unaligned_le64(header_nonce, (byte *) &header->nonce);
	put_unaligned_le64(virtual_chapter_number,
			   (byte *) &header->virtual_chapter_number);
	put_unaligned_le16(first_list, (byte *) &header->first_list);
	put_unaligned_le16(n_lists, (byte *) &header->num_lists);

	/* Construct the delta list offset table. */
	offset = get_immutable_header_offset(n_lists + 1);
	set_immutable_start(memory, 0, offset);
	for (i = 0; i < n_lists; i++) {
		offset += get_delta_list_size(&delta_lists[i]);
		set_immutable_start(memory, i + 1, offset);
	}

	/* Copy the delta list data onto the memory page. */
	for (i = 0; i < n_lists; i++) {
		move_bits(delta_zone->memory,
			  get_delta_list_start(&delta_lists[i]),
			  memory,
			  get_immutable_start(memory, i),
			  get_delta_list_size(&delta_lists[i]));
	}

	/* Set all the bits in the guard bytes. */
	memset(memory + memory_size - POST_FIELD_GUARD_BYTES,
	       ~0,
	       POST_FIELD_GUARD_BYTES);
	return UDS_SUCCESS;
}

#ifdef TEST_INTERNAL
/*
 * Given a delta index page written in little endian order, convert it into a
 * delta index page written in big endian order. Used to test old formats.
 */
void swap_delta_index_page_endianness(byte *memory)
{
	struct delta_page_header *header = (struct delta_page_header *) memory;
        const byte *nonce_addr = (const byte *) &header->nonce;
        const byte *vcn_addr = (const byte *) &header->virtual_chapter_number;
        const byte *first_list_addr = (const byte *) &header->first_list;
        const byte *num_lists_addr = (const byte *) &header->num_lists;
	uint64_t nonce = get_unaligned_le64(nonce_addr);
	uint64_t vcn = get_unaligned_le64(vcn_addr);
	uint16_t first_list = get_unaligned_le16(first_list_addr);
	uint16_t num_lists = get_unaligned_le16(num_lists_addr);

	put_unaligned_be64(nonce, (byte *) &header->nonce);
	put_unaligned_be64(vcn, (byte *) &header->virtual_chapter_number);
	put_unaligned_be16(first_list, (byte *) &header->first_list);
	put_unaligned_be16(num_lists, (byte *) &header->num_lists);
}

#endif /* TEST_INTERNAL */
void set_delta_index_tag(struct delta_index *delta_index, byte tag)
{
	unsigned int z;

	delta_index->tag = tag;
	for (z = 0; z < delta_index->num_zones; z++) {
		delta_index->delta_zones[z].tag = tag;
	}
}

static int __must_check decode_delta_index_header(struct buffer *buffer,
						  struct di_header *header)
{
	int result;

	result = get_bytes_from_buffer(buffer, MAGIC_SIZE, &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &header->zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &header->num_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint32_le_from_buffer(buffer, &header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &header->record_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &header->collision_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer) - content_length(buffer),
				 buffer_length(buffer));
	return result;
}

static int __must_check read_delta_index_header(struct buffered_reader *reader,
						struct di_header *header)
{
	int result;
	struct buffer *buffer;

	result = make_buffer(sizeof(*header), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_warning_strerror(result,
						"failed to read delta index header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_delta_index_header(buffer, header);
	free_buffer(UDS_FORGET(buffer));
	return result;
}

/* Compute the new offsets of the delta lists. */
static void compute_new_list_offsets(struct delta_memory *delta_memory,
				     unsigned int growing_index,
				     size_t growing_size,
				     size_t used_space)
{
	size_t spacing;
	unsigned int i;
	struct delta_list *delta_lists = delta_memory->delta_lists;
	unsigned int tail_guard_index = delta_memory->num_lists + 1;

	spacing = (delta_memory->size - used_space) / delta_memory->num_lists;
	delta_memory->temp_offsets[0] = 0;
	for (i = 0; i <= delta_memory->num_lists; i++) {
		delta_memory->temp_offsets[i + 1] =
			(delta_memory->temp_offsets[i] +
			 get_delta_list_byte_size(&delta_lists[i]) + spacing);
		delta_memory->temp_offsets[i] *= CHAR_BIT;
		delta_memory->temp_offsets[i] +=
			get_delta_list_start(&delta_lists[i]) % CHAR_BIT;
		if (i == 0) {
			delta_memory->temp_offsets[i + 1] -= spacing / 2;
		}
		if (i + 1 == growing_index) {
			delta_memory->temp_offsets[i + 1] += growing_size;
		}
	}

	delta_memory->temp_offsets[tail_guard_index] =
		(delta_memory->size * CHAR_BIT -
		 get_delta_list_size(&delta_lists[tail_guard_index]));
}

static void rebalance_lists(struct delta_memory *delta_memory)
{
	struct delta_list *delta_lists;
	unsigned int i;
	size_t used_space = 0;

	/* Extend and balance memory to receive the delta lists */
	delta_lists = delta_memory->delta_lists;
	for (i = 0; i <= delta_memory->num_lists + 1; i++) {
		used_space += get_delta_list_byte_size(&delta_lists[i]);
	}

	compute_new_list_offsets(delta_memory, 0, 0, used_space);
	for (i = 1; i <= delta_memory->num_lists + 1; i++) {
		delta_lists[i].start_offset = delta_memory->temp_offsets[i];
	}
}

/* Start restoring a delta index from multiple input streams. */
int start_restoring_delta_index(struct delta_index *delta_index,
				struct buffered_reader **buffered_readers,
				unsigned int num_readers)
{
	int result;
	unsigned int num_zones = num_readers;
	unsigned long record_count = 0;
	unsigned long collision_count = 0;
	unsigned int first_list[MAX_ZONES];
	unsigned int num_lists[MAX_ZONES];
	unsigned int z;
	unsigned int list_next = 0;
	const struct delta_memory *delta_zone;

	/* Read and validate each header. */
	for (z = 0; z < num_zones; z++) {
		struct di_header header;

		result = read_delta_index_header(buffered_readers[z], &header);
		if (result != UDS_SUCCESS) {
			return uds_log_warning_strerror(result,
							"failed to read delta index header");
		}

		if (memcmp(header.magic, MAGIC_DI_START, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index file has bad magic number");
		}

		if (num_zones != header.num_zones) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index files contain mismatched zone counts (%u,%u)",
							num_zones,
							header.num_zones);
		}

		if (header.zone_number >= num_zones) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index files contains zone %u of %u zones",
							header.zone_number,
							num_zones);
		}
		if (header.zone_number != z) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index zone %u found in slot %u",
							header.zone_number,
							z);
		}

		first_list[z] = header.first_list;
		num_lists[z] = header.num_lists;
		record_count += header.record_count;
		collision_count += header.collision_count;

		if (first_list[z] != list_next) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"delta index file for zone %u starts with list %u instead of list %u",
							z,
							first_list[z],
							list_next);
		}

		list_next += num_lists[z];
	}

	if (list_next != delta_index->num_lists) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"delta index files contain %u delta lists instead of %u delta lists",
						list_next,
						delta_index->num_lists);
	}

	if (collision_count > record_count) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"delta index files contain %ld collisions and %ld records",
						collision_count,
						record_count);
	}

	empty_delta_index(delta_index);
	delta_index->delta_zones[0].record_count = record_count;
	delta_index->delta_zones[0].collision_count = collision_count;

	/* Read the delta lists and distribute them to the proper zones. */
	for (z = 0; z < num_zones; z++) {
		unsigned int i;

		delta_index->load_lists[z] = 0;
		for (i = 0; i < num_lists[z]; i++) {
			uint16_t delta_list_size;
			unsigned int list_number;
			unsigned int zone_number;
			byte size_data[sizeof(uint16_t)];

			result = read_from_buffered_reader(buffered_readers[z],
							   size_data,
							   sizeof(size_data));
			if (result != UDS_SUCCESS) {
				return uds_log_warning_strerror(result,
								"failed to read delta index size");
			}

			delta_list_size = get_unaligned_le16(size_data);
			if (delta_list_size > 0) {
				delta_index->load_lists[z] += 1;
			}

			list_number = first_list[z] + i;
			zone_number =
				get_delta_index_zone(delta_index, list_number);
			delta_zone = &delta_index->delta_zones[zone_number];
			list_number -= delta_zone->first_list;
			delta_zone->delta_lists[list_number + 1].size =
				delta_list_size;
		}
	}

	/* Prepare each zone to start receiving the delta list data. */
	for (z = 0; z < delta_index->num_zones; z++) {
		rebalance_lists(&delta_index->delta_zones[z]);
	}
	return UDS_SUCCESS;
}

static int restore_delta_list_to_zone(struct delta_memory *delta_memory,
				      const struct delta_list_save_info *dlsi,
				      const byte *data)
{
	struct delta_list *delta_list;
	uint16_t bit_size;
	unsigned int byte_count;
	unsigned int list_number = dlsi->index - delta_memory->first_list;

	if (list_number >= delta_memory->num_lists) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"invalid delta list number %u not in range [%u,%u)",
						dlsi->index,
						delta_memory->first_list,
						delta_memory->first_list +
						delta_memory->num_lists);
	}

	delta_list = &delta_memory->delta_lists[list_number + 1];
	bit_size = get_delta_list_size(delta_list);
	if (bit_size == 0) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"unexpected delta list number %u",
						dlsi->index);
	}

	byte_count = DIV_ROUND_UP((unsigned int) dlsi->bit_offset + bit_size,
				  CHAR_BIT);
	if (dlsi->byte_count != byte_count) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"unexpected delta list size %u != %u",
						dlsi->byte_count,
						byte_count);
	}

	move_bits(data,
		  dlsi->bit_offset,
		  delta_memory->memory,
		  get_delta_list_start(delta_list),
		  bit_size);
	return UDS_SUCCESS;
}

static int __must_check
read_delta_list_save_info(struct buffered_reader *reader,
			  struct delta_list_save_info *dlsi)
{
	int result;
	byte buffer[sizeof(struct delta_list_save_info)];

	result = read_from_buffered_reader(reader, buffer, sizeof(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	dlsi->tag = buffer[0];
	dlsi->bit_offset = buffer[1];
	dlsi->byte_count = get_unaligned_le16(&buffer[2]);
	dlsi->index = get_unaligned_le32(&buffer[4]);
	return result;
}

static int read_saved_delta_list(struct delta_list_save_info *dlsi,
				 struct buffered_reader *buffered_reader)
{
	int result;

	result = read_delta_list_save_info(buffered_reader, dlsi);
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to read delta list data");
	}

	if ((dlsi->bit_offset >= CHAR_BIT) ||
	    (dlsi->byte_count > DELTA_LIST_MAX_BYTE_COUNT)) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"corrupt delta list data");
	}

	return UDS_SUCCESS;
}

static int restore_delta_list_data(struct delta_index *delta_index,
				   unsigned int load_zone,
				   struct buffered_reader *buffered_reader,
				   byte *data)
{
	int result;
	struct delta_list_save_info dlsi = { 0 };
	unsigned int new_zone;

	result = read_saved_delta_list(&dlsi, buffered_reader);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* Make sure the data is intended for this delta index. */
	if (dlsi.tag != delta_index->tag) {
		return UDS_CORRUPT_DATA;
	}

	if (dlsi.index >= delta_index->num_lists) {
		return uds_log_warning_strerror(UDS_CORRUPT_DATA,
						"invalid delta list number %u of %u",
						dlsi.index,
						delta_index->num_lists);
	}

	result = read_from_buffered_reader(buffered_reader, data,
					   dlsi.byte_count);
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to read delta list data");
	}

	delta_index->load_lists[load_zone] -= 1;
	new_zone = get_delta_index_zone(delta_index, dlsi.index);
	return restore_delta_list_to_zone(&delta_index->delta_zones[new_zone],
					  &dlsi,
					  data);
}

/* Restore delta lists from saved data. */
int finish_restoring_delta_index(struct delta_index *delta_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int num_readers)
{
	int result;
	int saved_result = UDS_SUCCESS;
	unsigned int z;
	byte *data;

	result = UDS_ALLOCATE(DELTA_LIST_MAX_BYTE_COUNT,
			      byte,
			      __func__,
			      &data);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (z = 0; z < num_readers; z++) {
		while (delta_index->load_lists[z] > 0) {
			result = restore_delta_list_data(delta_index,
							 z,
							 buffered_readers[z],
							 data);
			if (result != UDS_SUCCESS) {
				saved_result = result;
				break;
			}
		}
	}

	UDS_FREE(data);
	return saved_result;
}

void abort_restoring_delta_index(const struct delta_index *delta_index)
{
	unsigned int z;

	for (z = 0; z < delta_index->num_zones; z++) {
		empty_delta_lists(&delta_index->delta_zones[z]);
	}
}

int check_guard_delta_lists(struct buffered_reader **buffered_readers,
			    unsigned int num_readers)
{
	int result;
	unsigned int z;
	struct delta_list_save_info dlsi;

	for (z = 0; z < num_readers; z++) {
		result = read_delta_list_save_info(buffered_readers[z], &dlsi);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (dlsi.tag != 'z') {
			return UDS_CORRUPT_DATA;
		}
	}

	return UDS_SUCCESS;
}

static int __must_check encode_delta_index_header(struct buffer *buffer,
						  struct di_header *header)
{
	int result;

	result = put_bytes(buffer, MAGIC_SIZE, MAGIC_DI_START);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, header->zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, header->num_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, header->first_list);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint32_le_into_buffer(buffer, header->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, header->record_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, header->collision_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return ASSERT_LOG_ONLY(content_length(buffer) == sizeof(*header),
			       "%zu bytes encoded of %zu expected",
			       content_length(buffer),
			       sizeof(*header));
}

static int __must_check
write_delta_list_save_info(struct buffered_writer *buffered_writer,
			   struct delta_list_save_info *dlsi)
{
	byte buffer[sizeof(struct delta_list_save_info)];

	buffer[0] = dlsi->tag;
	buffer[1] = dlsi->bit_offset;
	put_unaligned_le16(dlsi->byte_count, &buffer[2]);
	put_unaligned_le32(dlsi->index, &buffer[4]);
	return write_to_buffered_writer(buffered_writer, buffer,
					sizeof(buffer));
}

static int flush_delta_list(struct delta_memory *delta_memory,
			    unsigned int flush_index)
{
	struct delta_list *delta_list;
	struct delta_list_save_info dlsi;
	int result;

	delta_list = &delta_memory->delta_lists[flush_index + 1];
	dlsi.tag = delta_memory->tag;
	dlsi.bit_offset = get_delta_list_start(delta_list) % CHAR_BIT;
	dlsi.byte_count = get_delta_list_byte_size(delta_list);
	dlsi.index = delta_memory->first_list + flush_index;

	result = write_delta_list_save_info(delta_memory->buffered_writer,
						&dlsi);
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "failed to write delta list memory");
		return result;
	}

	result = write_to_buffered_writer(delta_memory->buffered_writer,
		delta_memory->memory + get_delta_list_byte_start(delta_list),
		dlsi.byte_count);
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "failed to write delta list memory");
	}

	return result;
}

/* Start saving a delta index zone to a buffered output stream. */
int start_saving_delta_index(const struct delta_index *delta_index,
			     unsigned int zone_number,
			     struct buffered_writer *buffered_writer)
{
	int result;
	unsigned int i;
	struct buffer *buffer;
	struct delta_memory *delta_zone;
	struct di_header header;

	delta_zone = &delta_index->delta_zones[zone_number];
	memcpy(header.magic, MAGIC_DI_START, MAGIC_SIZE);
	header.zone_number = zone_number;
	header.num_zones = delta_index->num_zones;
	header.first_list = delta_zone->first_list;
	header.num_lists = delta_zone->num_lists;
	header.record_count = delta_zone->record_count;
	header.collision_count = delta_zone->collision_count;

	result = make_buffer(sizeof(struct di_header), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_delta_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"failed to write delta index header");
	}

	for (i = 0; i < delta_zone->num_lists; i++) {
		byte data[sizeof(uint16_t)];
		struct delta_list *delta_list;

		delta_list = &delta_zone->delta_lists[i + 1];
		put_unaligned_le16(get_delta_list_size(delta_list), data);
		result = write_to_buffered_writer(buffered_writer,
						  data,
						  sizeof(data));
		if (result != UDS_SUCCESS) {
			return uds_log_warning_strerror(result,
							"failed to write delta list size");
		}
	}

	delta_zone->buffered_writer = buffered_writer;
	return UDS_SUCCESS;
}

int finish_saving_delta_index(const struct delta_index *delta_index,
			      unsigned int zone_number)
{
	int result;
	int first_error = UDS_SUCCESS;
	unsigned int i;
	struct delta_memory *delta_memory;
	struct delta_list *delta_list;

        delta_memory = &delta_index->delta_zones[zone_number];
	for (i = 0; i < delta_memory->num_lists;i++) {
		delta_list = &delta_memory->delta_lists[i + 1];
		if (get_delta_list_size(delta_list) > 0) {
			result = flush_delta_list(delta_memory, i);
			if ((result != UDS_SUCCESS) &&
			    (first_error == UDS_SUCCESS)) {
				first_error = result;
			}
		}
	}

	delta_memory->buffered_writer = NULL;
	return first_error;
}

int write_guard_delta_list(struct buffered_writer *buffered_writer)
{
	int result;
	struct delta_list_save_info dlsi;

	dlsi.tag = 'z';
	dlsi.bit_offset = 0;
	dlsi.byte_count = 0;
	dlsi.index = 0;
	result = write_to_buffered_writer(buffered_writer,
					  (const byte *) &dlsi,
					  sizeof(struct delta_list_save_info));
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "failed to write guard delta list");
	}
	return result;
}

size_t compute_delta_index_save_bytes(unsigned int num_lists,
				      size_t memory_size)
{
	/* One zone will use at least as much memory as other zone counts. */
	return (sizeof(struct di_header)
		+ num_lists * (sizeof(struct delta_list_save_info) + 1)
		+ get_zone_memory_size(1, memory_size));
}

static int assert_not_at_end(const struct delta_index_entry *delta_entry)
{
	return ASSERT_WITH_ERROR_CODE(!delta_entry->at_end,
				      UDS_BAD_STATE,
		                      "operation is invalid because the list entry is at the end of the delta list");
}

static void prefetch_delta_list(const struct delta_memory *delta_zone,
				const struct delta_list *delta_list)
{
	uint64_t memory_offset = get_delta_list_start(delta_list) / CHAR_BIT;
	const byte *addr = &delta_zone->memory[memory_offset];
	unsigned int size = get_delta_list_size(delta_list) / CHAR_BIT;

	prefetch_range(addr, size, false);
}

/*
 * Prepare to search for an entry in the specified delta list.
 *
 * This is always the first function to be called when dealing with delta index
 * entries. It is always followed by calls to next_delta_index_entry() to
 * iterate through a delta list. The fields of the delta_index_entry argument
 * will be set up for iteration, but will not contain an entry from the list.
 */
int start_delta_index_search(const struct delta_index *delta_index,
			     unsigned int list_number,
			     unsigned int key,
			     struct delta_index_entry *delta_entry)
{
	int result;
	unsigned int zone_number;
	struct delta_memory *delta_zone;
	struct delta_list *delta_list;

	result = ASSERT_WITH_ERROR_CODE((list_number < delta_index->num_lists),
					UDS_CORRUPT_DATA,
					"Delta list number (%u) is out of range (%u)",
					list_number,
					delta_index->num_lists);
	if (result != UDS_SUCCESS) {
		return result;
	}

	zone_number = get_delta_index_zone(delta_index, list_number);
	delta_zone = &delta_index->delta_zones[zone_number];
	list_number -= delta_zone->first_list;
	result = ASSERT_WITH_ERROR_CODE((list_number < delta_zone->num_lists),
					UDS_CORRUPT_DATA,
					"Delta list number (%u) is out of range (%u) for zone (%u)",
					list_number,
					delta_zone->num_lists,
					zone_number);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (delta_index->is_mutable) {
		delta_list = &delta_zone->delta_lists[list_number + 1];
	} else {
		unsigned int end_offset;

		/*
		 * Translate the immutable delta list header into a temporary
		 * full delta list header.
		 */
		delta_list = &delta_entry->temp_delta_list;
		delta_list->start_offset =
			get_immutable_start(delta_zone->memory, list_number);
		end_offset = get_immutable_start(delta_zone->memory,
						 list_number + 1);
		delta_list->size = end_offset - delta_list->start_offset;
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}

	if (key > delta_list->save_key) {
		delta_entry->key = delta_list->save_key;
		delta_entry->offset = delta_list->save_offset;
	} else {
		delta_entry->key = 0;
		delta_entry->offset = 0;
		if (key == 0) {
			/*
			 * This usually means we're about to walk the entire
			 * delta list, so get all of it into the CPU cache.
			 */
			prefetch_delta_list(delta_zone, delta_list);
		}
	}

	delta_entry->at_end = false;
	delta_entry->delta_zone = delta_zone;
	delta_entry->delta_list = delta_list;
	delta_entry->entry_bits = 0;
	delta_entry->is_collision = false;
	delta_entry->list_number = list_number;
	delta_entry->list_overflow = false;
	delta_entry->value_bits = delta_zone->value_bits;
	return UDS_SUCCESS;
}

static INLINE uint64_t
get_delta_entry_offset(const struct delta_index_entry *delta_entry)
{
	return (get_delta_list_start(delta_entry->delta_list) +
		delta_entry->offset);
}

/*
 * Decode a delta index entry delta value. The delta_index_entry basically
 * describes the previous list entry, and has had its offset field changed to
 * point to the subsequent entry. We decode the bit stream and update the
 * delta_list_entry to describe the entry.
 */
static INLINE void decode_delta(struct delta_index_entry *delta_entry)
{
	int key_bits;
	unsigned int delta;
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	const byte *memory = delta_zone->memory;
	uint64_t delta_offset =
		get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	const byte *addr = memory + delta_offset / CHAR_BIT;
	int offset = delta_offset % CHAR_BIT;
	uint32_t data = get_unaligned_le32(addr) >> offset;

	addr += sizeof(uint32_t);
	key_bits = delta_zone->min_bits;
	delta = data & ((1 << key_bits) - 1);
	if (delta >= delta_zone->min_keys) {
		data >>= key_bits;
		if (data == 0) {
			key_bits = sizeof(uint32_t) * CHAR_BIT - offset;
			while ((data = get_unaligned_le32(addr)) == 0) {
				addr += sizeof(uint32_t);
				key_bits += sizeof(uint32_t) * CHAR_BIT;
			}
		}
		key_bits += ffs(data);
		delta += ((key_bits - delta_zone->min_bits - 1) *
			  delta_zone->incr_keys);
	}
	delta_entry->delta = delta;
	delta_entry->key += delta;

	/* Check for a collision, a delta of zero after the start. */
	if (unlikely((delta == 0) && (delta_entry->offset > 0))) {
		delta_entry->is_collision = true;
		delta_entry->entry_bits =
			delta_entry->value_bits + key_bits + COLLISION_BITS;
	} else {
		delta_entry->is_collision = false;
		delta_entry->entry_bits = delta_entry->value_bits + key_bits;
	}
}

noinline int next_delta_index_entry(struct delta_index_entry *delta_entry)
{
	int result;
	const struct delta_list *delta_list;
	unsigned int next_offset;
	unsigned int size;

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_list = delta_entry->delta_list;
	delta_entry->offset += delta_entry->entry_bits;
	size = get_delta_list_size(delta_list);
	if (unlikely(delta_entry->offset >= size)) {
		delta_entry->at_end = true;
		delta_entry->delta = 0;
		delta_entry->is_collision = false;
		return ASSERT_WITH_ERROR_CODE((delta_entry->offset == size),
					      UDS_CORRUPT_DATA,
					      "next offset past end of delta list");
	}

	decode_delta(delta_entry);

	next_offset = delta_entry->offset + delta_entry->entry_bits;
	if (next_offset > size) {
		/*
		 * This is not an assertion because
		 * validate_chapter_index_page() wants to handle this error.
		 */
		uds_log_warning("Decoded past the end of the delta list");
		return UDS_CORRUPT_DATA;
	}

	return UDS_SUCCESS;
}

int remember_delta_index_offset(const struct delta_index_entry *delta_entry)
{
	int result;
	struct delta_list *delta_list = delta_entry->delta_list;

	result = ASSERT(!delta_entry->is_collision,
			"entry is not a collision");
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_list->save_key = delta_entry->key - delta_entry->delta;
	delta_list->save_offset = delta_entry->offset;
	return UDS_SUCCESS;
}

static void set_delta(struct delta_index_entry *delta_entry, unsigned int delta)
{
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	int key_bits = (delta_zone->min_bits +
			((delta_zone->incr_keys -
			  delta_zone->min_keys + delta) /
			 delta_zone->incr_keys));

	delta_entry->delta = delta;
	delta_entry->entry_bits = delta_entry->value_bits + key_bits;
}

static void set_collision(struct delta_index_entry *delta_entry)
{
	delta_entry->is_collision = true;
	delta_entry->entry_bits += COLLISION_BITS;
}

/* Get the bit offset of the collision field of an entry. */
static INLINE uint64_t
get_collision_offset(const struct delta_index_entry *entry)
{
	return (get_delta_entry_offset(entry) + entry->entry_bits -
		COLLISION_BITS);
}

static void get_collision_name(const struct delta_index_entry *entry,
			       byte *name)
{
	uint64_t offset = get_collision_offset(entry);
	const byte *addr = entry->delta_zone->memory + offset / CHAR_BIT;
	int size = COLLISION_BYTES;
	int shift = offset % CHAR_BIT;

	while (--size >= 0) {
		*name++ = get_unaligned_le16(addr++) >> shift;
	}
}

static void set_collision_name(const struct delta_index_entry *entry,
			       const byte *name)
{
	uint64_t offset = get_collision_offset(entry);
	byte *addr = entry->delta_zone->memory + offset / CHAR_BIT;
	int size = COLLISION_BYTES;
	int shift = offset % CHAR_BIT;
	uint16_t mask = ~((uint16_t) 0xFF << shift);
	uint16_t data;

	while (--size >= 0) {
		data = (get_unaligned_le16(addr) & mask) | (*name++ << shift);
		put_unaligned_le16(data, addr++);
	}
}

int get_delta_index_entry(const struct delta_index *delta_index,
			  unsigned int list_number,
			  unsigned int key,
			  const byte *name,
			  struct delta_index_entry *delta_entry)
{
	int result;

	result = start_delta_index_search(delta_index,
					  list_number,
					  key,
					  delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	do {
		result = next_delta_index_entry(delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} while (!delta_entry->at_end && (key > delta_entry->key));

	result = remember_delta_index_offset(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (!delta_entry->at_end && (key == delta_entry->key)) {
		struct delta_index_entry collision_entry;

		collision_entry = *delta_entry;
		for (;;) {
			byte full_name[COLLISION_BYTES];

			result = next_delta_index_entry(&collision_entry);
			if (result != UDS_SUCCESS) {
				return result;
			}

			if (collision_entry.at_end ||
			    !collision_entry.is_collision) {
				break;
			}

			get_collision_name(&collision_entry, full_name);
			if (memcmp(full_name, name, COLLISION_BYTES) == 0) {
				*delta_entry = collision_entry;
				break;
			}
		}
	}

	return UDS_SUCCESS;
}

int get_delta_entry_collision(const struct delta_index_entry *delta_entry,
			      byte *name)
{
	int result;

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT_WITH_ERROR_CODE(delta_entry->is_collision,
					UDS_BAD_STATE,
					"Cannot get full block name from a non-collision delta index entry");
	if (result != UDS_SUCCESS) {
		return result;
	}

	get_collision_name(delta_entry, name);
	return UDS_SUCCESS;
}

unsigned int get_delta_entry_value(const struct delta_index_entry *delta_entry)
{
	return get_field(delta_entry->delta_zone->memory,
			 get_delta_entry_offset(delta_entry),
			 delta_entry->value_bits);
}

static int assert_mutable_entry(const struct delta_index_entry *delta_entry)
{
	return ASSERT_WITH_ERROR_CODE((delta_entry->delta_list !=
				       &delta_entry->temp_delta_list),
				      UDS_BAD_STATE,
				      "delta index is mutable");
}

int set_delta_entry_value(const struct delta_index_entry *delta_entry,
			  unsigned int value)
{
	int result;
	unsigned int value_mask = (1 << delta_entry->value_bits) - 1;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = assert_not_at_end(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT_WITH_ERROR_CODE((value & value_mask) == value,
					UDS_INVALID_ARGUMENT,
					"Value (%u) being set in a delta index is too large (must fit in %u bits)",
					value,
					delta_entry->value_bits);
	if (result != UDS_SUCCESS) {
		return result;
	}

	set_field(value,
		  delta_entry->delta_zone->memory,
		  get_delta_entry_offset(delta_entry),
		  delta_entry->value_bits);
	return UDS_SUCCESS;
}

/*
 * Extend the memory used by the delta lists by adding growing_size
 * bytes before the list indicated by growing_index, then rebalancing
 * the lists in the new chunk.
 */
EXTERNAL_STATIC int extend_delta_memory(struct delta_memory *delta_memory,
					unsigned int growing_index,
					size_t growing_size)
{
	ktime_t start_time;
	ktime_t end_time;
	struct delta_list *delta_lists;
	unsigned int i;
	size_t used_space;


	/* Calculate the amount of space that is or will be in use. */
	start_time = current_time_ns(CLOCK_MONOTONIC);
	delta_lists = delta_memory->delta_lists;
	used_space = growing_size;
	for (i = 0; i <= delta_memory->num_lists + 1; i++) {
		used_space += get_delta_list_byte_size(&delta_lists[i]);
	}

	if (delta_memory->size < used_space) {
		return UDS_OVERFLOW;
	}

	/* Compute the new offsets of the delta lists. */
	compute_new_list_offsets(delta_memory,
				 growing_index,
				 growing_size,
				 used_space);

	/*
	 * When we rebalance the delta list, we will include the end guard list
	 * in the rebalancing. It contains the end guard data, which must be
	 * copied.
	 */
	rebalance_delta_memory(delta_memory, 1, delta_memory->num_lists + 1);
	end_time = current_time_ns(CLOCK_MONOTONIC);
	delta_memory->rebalance_count++;
	delta_memory->rebalance_time += ktime_sub(end_time, start_time);
	return UDS_SUCCESS;
}

static int insert_bits(struct delta_index_entry *delta_entry, int size)
{
	uint64_t free_before, free_after, source, destination;
	uint32_t count;
	bool before_flag;
	byte *memory;
	struct delta_memory *delta_zone = delta_entry->delta_zone;
	struct delta_list *delta_list = delta_entry->delta_list;
	/* Compute bits in use before and after the inserted bits. */
	uint32_t total_size = get_delta_list_size(delta_list);
	uint32_t before_size = delta_entry->offset;
	uint32_t after_size = total_size - delta_entry->offset;

	if ((unsigned int) (total_size + size) > UINT16_MAX) {
		delta_entry->list_overflow = true;
		delta_zone->overflow_count++;
		return UDS_OVERFLOW;
	}

	/* Compute bits available before and after the delta list. */
	free_before = (get_delta_list_start(&delta_list[0]) -
		       get_delta_list_end(&delta_list[-1]));
	free_after = (get_delta_list_start(&delta_list[1]) -
		      get_delta_list_end(&delta_list[0]));

	if (((unsigned int) size <= free_before) &&
	    ((unsigned int) size <= free_after)) {
		/*
		 * We have enough space to use either before or after the list.
		 * Select the smaller amount of data. If it is exactly the
		 * same, try to take from the larger amount of free space.
		 */
		if (before_size < after_size) {
			before_flag = true;
		} else if (after_size < before_size) {
			before_flag = false;
		} else {
			before_flag = free_before > free_after;
		}
	} else if ((unsigned int) size <= free_before) {
		/* There is space before but not after. */
		before_flag = true;
	} else if ((unsigned int) size <= free_after) {
		/* There is space after but not before. */
		before_flag = false;
	} else {
		/*
		 * Neither of the surrounding spaces is large enough for this
		 * request. Extend and/or rebalance the delta list memory
		 * choosing to move the least amount of data.
		 */
		int result;
		unsigned int growing_index = delta_entry->list_number + 1;

		before_flag = before_size < after_size;
		if (!before_flag) {
			growing_index++;
		}
		result = extend_delta_memory(delta_zone,
					     growing_index,
					     DIV_ROUND_UP(size, CHAR_BIT));
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (before_flag) {
		source = get_delta_list_start(delta_list);
		destination = source - size;
		move_delta_list_start(delta_list, -size);
		count = before_size;
	} else {
		move_delta_list_end(delta_list, size);
		source = (get_delta_list_start(delta_list) +
			  delta_entry->offset);
		destination = source + size;
		count = after_size;
	}

	memory = delta_zone->memory;
	move_bits(memory, source, memory, destination, count);
	return UDS_SUCCESS;
}

static void encode_delta(const struct delta_index_entry *delta_entry)
{
	unsigned int temp, t1, t2;
	uint64_t offset;
	const struct delta_memory *delta_zone = delta_entry->delta_zone;
	byte *memory = delta_zone->memory;

	offset = get_delta_entry_offset(delta_entry) + delta_entry->value_bits;
	if (delta_entry->delta < delta_zone->min_keys) {
		set_field(delta_entry->delta,
			  memory,
			  offset,
			  delta_zone->min_bits);
		return;
	}

	temp = delta_entry->delta - delta_zone->min_keys;
	t1 = (temp % delta_zone->incr_keys) + delta_zone->min_keys;
	t2 = temp / delta_zone->incr_keys;
	set_field(t1, memory, offset, delta_zone->min_bits);
	set_zero(memory, offset + delta_zone->min_bits, t2);
	set_field(1, memory, offset + delta_zone->min_bits + t2, 1);
}

static void encode_entry(const struct delta_index_entry *delta_entry,
			 unsigned int value,
			 const byte *name)
{
	byte *memory = delta_entry->delta_zone->memory;
	uint64_t offset = get_delta_entry_offset(delta_entry);

	set_field(value, memory, offset, delta_entry->value_bits);
	encode_delta(delta_entry);
	if (name != NULL) {
		set_collision_name(delta_entry, name);
	}
}

/*
 * Create a new entry in the delta index. If the entry is a collision, the full
 * 256 bit name must be provided.
 */
int put_delta_index_entry(struct delta_index_entry *delta_entry,
			  unsigned int key,
			  unsigned int value,
			  const byte *name)
{
	int result;
	struct delta_memory *delta_zone;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (delta_entry->is_collision) {
		/*
		 * The caller wants us to insert a collision entry onto a
		 * collision entry. This happens when we find a collision and
		 * attempt to add the name again to the index. This is
		 * normally a fatal error unless we are replaying a closed
		 * chapter while we are rebuilding a volume index.
		 */
		return UDS_DUPLICATE_NAME;
	}

	if (delta_entry->offset < delta_entry->delta_list->save_offset) {
		/*
		 * The saved entry offset is after the new entry and will no
		 * longer be valid, so replace it with the insertion point.
		 */
		result = remember_delta_index_offset(delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (name != NULL) {
		/*
		 * Insert a collision entry which is placed after this
		 * entry.
		 */
		result = assert_not_at_end(delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = ASSERT((key == delta_entry->key),
				"incorrect key for collision entry");
		if (result != UDS_SUCCESS) {
			return result;
		}

		delta_entry->offset += delta_entry->entry_bits;
		set_delta(delta_entry, 0);
		set_collision(delta_entry);
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else if (delta_entry->at_end) {
		/* Insert a new entry at the end of the delta list. */
		result = ASSERT((key >= delta_entry->key),
				"key past end of list");
		if (result != UDS_SUCCESS) {
			return result;
		}

		set_delta(delta_entry, key - delta_entry->key);
		delta_entry->key = key;
		delta_entry->at_end = false;
		result = insert_bits(delta_entry, delta_entry->entry_bits);
	} else {
		int old_entry_size;
		int additional_size;
		struct delta_index_entry next_entry;
		unsigned int next_value;

		/*
		 * Insert a new entry which requires the delta in the following
		 * entry to be updated.
		 */
		result = ASSERT((key < delta_entry->key),
				"key precedes following entry");
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = ASSERT((key >= delta_entry->key - delta_entry->delta),
				"key effects following entry's delta");
		if (result != UDS_SUCCESS) {
			return result;
		}

		old_entry_size = delta_entry->entry_bits;
		next_entry = *delta_entry;
		next_value = get_delta_entry_value(&next_entry);
		set_delta(delta_entry,
			  key - (delta_entry->key - delta_entry->delta));
		delta_entry->key = key;
		set_delta(&next_entry, next_entry.key - key);
		next_entry.offset += delta_entry->entry_bits;
		/*
		 * The two new entries are always bigger than the single entry
		 * being replaced.
		 */
		additional_size = (delta_entry->entry_bits +
				   next_entry.entry_bits - old_entry_size);
		result = insert_bits(delta_entry, additional_size);
		if (result != UDS_SUCCESS) {
			return result;
		}

		encode_entry(&next_entry, next_value, NULL);
	}

	if (result != UDS_SUCCESS) {
		return result;
	}

	encode_entry(delta_entry, value, name);
	delta_zone = delta_entry->delta_zone;
	delta_zone->record_count++;
	delta_zone->collision_count += delta_entry->is_collision ? 1 : 0;
	return UDS_SUCCESS;
}

static void delete_bits(const struct delta_index_entry *delta_entry, int size)
{
	uint64_t source, destination;
	uint32_t count;
	bool before_flag;
	struct delta_list *delta_list = delta_entry->delta_list;
	byte *memory = delta_entry->delta_zone->memory;
	/* Compute bits retained before and after the deleted bits. */
	uint32_t total_size = get_delta_list_size(delta_list);
	uint32_t before_size = delta_entry->offset;
	uint32_t after_size = total_size - delta_entry->offset - size;

	/*
	 * Determine whether to add to the available space either before or
	 * after the delta list. We prefer to move the least amount of data.
	 * If it is exactly the same, try to add to the smaller amount of free
	 * space.
	 */
	if (before_size < after_size) {
		before_flag = true;
	} else if (after_size < before_size) {
		before_flag = false;
	} else {
		uint64_t free_before = (get_delta_list_start(&delta_list[0]) -
					get_delta_list_end(&delta_list[-1]));
		uint64_t free_after = (get_delta_list_start(&delta_list[1]) -
				       get_delta_list_end(&delta_list[0]));

		before_flag = (free_before < free_after);
	}

	if (before_flag) {
		source = get_delta_list_start(delta_list);
		destination = source + size;
		move_delta_list_start(delta_list, size);
		count = before_size;
	} else {
		move_delta_list_end(delta_list, -size);
		destination =
			get_delta_list_start(delta_list) + delta_entry->offset;
		source = destination + size;
		count = after_size;
	}

	move_bits(memory, source, memory, destination, count);
}

int remove_delta_index_entry(struct delta_index_entry *delta_entry)
{
	int result;
	struct delta_index_entry next_entry;
	struct delta_memory *delta_zone;
	struct delta_list *delta_list;

	result = assert_mutable_entry(delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_entry = *delta_entry;
	result = next_delta_index_entry(&next_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	delta_zone = delta_entry->delta_zone;

	if (delta_entry->is_collision) {
		/* This is a collision entry, so just remove it. */
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.offset = delta_entry->offset;
		delta_zone->collision_count -= 1;
	} else if (next_entry.at_end) {
		/* This entry is at the end of the list, so just remove it. */
		delete_bits(delta_entry, delta_entry->entry_bits);
		next_entry.key -= delta_entry->delta;
		next_entry.offset = delta_entry->offset;
	} else {
		/* The delta in the next entry needs to be updated. */
		unsigned int next_value = get_delta_entry_value(&next_entry);
		int old_size = delta_entry->entry_bits + next_entry.entry_bits;

		if (next_entry.is_collision) {
			next_entry.is_collision = false;
			delta_zone->collision_count -= 1;
		}

		set_delta(&next_entry, delta_entry->delta + next_entry.delta);
		next_entry.offset = delta_entry->offset;
		/*
		 * The one new entry is always smaller than the two entries
		 * being replaced.
		 */
		delete_bits(delta_entry, old_size - next_entry.entry_bits);
		encode_entry(&next_entry, next_value, NULL);
	}

	delta_zone->record_count--;
	delta_zone->discard_count++;
	*delta_entry = next_entry;

	delta_list = delta_entry->delta_list;
	if (delta_entry->offset < delta_list->save_offset) {
		/* The saved entry offset is no longer valid. */
		delta_list->save_key = 0;
		delta_list->save_offset = 0;
	}

	return UDS_SUCCESS;
}

unsigned int
get_delta_index_zone_first_list(const struct delta_index *delta_index,
				unsigned int zone_number)
{
	return delta_index->delta_zones[zone_number].first_list;
}

unsigned int
get_delta_index_zone_num_lists(const struct delta_index *delta_index,
			       unsigned int zone_number)
{
	return delta_index->delta_zones[zone_number].num_lists;
}

uint64_t
get_delta_index_zone_dlist_bits_used(const struct delta_index *delta_index,
				     unsigned int zone_number)
{
	unsigned int i;
	uint64_t bit_count = 0;
	const struct delta_memory *delta_zone;

	delta_zone = &delta_index->delta_zones[zone_number];
	for (i = 0; i < delta_zone->num_lists; i++) {
		bit_count +=
			get_delta_list_size(&delta_zone->delta_lists[i + 1]);
	}

	return bit_count;
}

#ifdef TEST_INTERNAL
uint64_t get_delta_index_dlist_bits_used(const struct delta_index *delta_index)
{
	uint64_t bit_count = 0;
	unsigned int z;

	for (z = 0; z < delta_index->num_zones; z++) {
		bit_count +=
			get_delta_index_zone_dlist_bits_used(delta_index, z);
	}
	return bit_count;
}

#endif /* TEST_INTERNAL */
uint64_t
get_delta_index_dlist_bits_allocated(const struct delta_index *delta_index)
{
	uint64_t byte_count = 0;
	unsigned int z;

	for (z = 0; z < delta_index->num_zones; z++) {
		byte_count += delta_index->delta_zones[z].size;
	}

	return byte_count * CHAR_BIT;
}

static size_t
get_delta_memory_allocated(const struct delta_memory *delta_memory)
{
	return (delta_memory->size +
		(delta_memory->num_lists + 2) * sizeof(struct delta_list) +
		(delta_memory->num_lists + 2) * sizeof(uint64_t));
}

void get_delta_index_stats(const struct delta_index *delta_index,
			   struct delta_index_stats *stats)
{
	unsigned int z;
	const struct delta_memory *delta_zone;

	memset(stats, 0, sizeof(struct delta_index_stats));
	stats->memory_allocated =
		delta_index->num_zones * sizeof(struct delta_memory);
	for (z = 0; z < delta_index->num_zones; z++) {
		delta_zone = &delta_index->delta_zones[z];
		stats->memory_allocated +=
			get_delta_memory_allocated(delta_zone);
		stats->rebalance_time += delta_zone->rebalance_time;
		stats->rebalance_count += delta_zone->rebalance_count;
		stats->record_count += delta_zone->record_count;
		stats->collision_count += delta_zone->collision_count;
		stats->discard_count += delta_zone->discard_count;
		stats->overflow_count += delta_zone->overflow_count;
		stats->num_lists += delta_zone->num_lists;
	}
}

size_t get_delta_memory_size(unsigned long num_entries,
			     unsigned int mean_delta,
			     unsigned int num_payload_bits)
{
	unsigned short min_bits;
	unsigned int incr_keys, min_keys;

	compute_coding_constants(mean_delta, &min_bits, &min_keys, &incr_keys);
	/* On average, each delta is encoded into about min_bits + 1.5 bits. */
	return (num_entries * (num_payload_bits + min_bits + 1) +
		num_entries / 2);
}

unsigned int get_delta_index_page_count(unsigned int num_entries,
					unsigned int num_lists,
					unsigned int mean_delta,
					unsigned int num_payload_bits,
					size_t bytes_per_page)
{
	unsigned int bits_per_delta_list;
	unsigned int bits_per_page;
	size_t bits_per_index;

	/* Compute the expected number of bits needed for all the entries. */
	bits_per_index = get_delta_memory_size(num_entries,
					       mean_delta,
					       num_payload_bits);
	bits_per_delta_list = bits_per_index / num_lists;

	/* Add in the immutable delta list headers. */
	bits_per_index += num_lists * IMMUTABLE_HEADER_SIZE;
	/* Compute the number of usable bits on an immutable index page. */
	bits_per_page = ((bytes_per_page - sizeof(struct delta_page_header)) *
			 CHAR_BIT);
	/*
	 * Reduce the bits per page by one immutable delta list header and one
	 * delta list to account for internal fragmentation.
	 */
	bits_per_page -= IMMUTABLE_HEADER_SIZE + bits_per_delta_list;
	/* Now compute the number of pages needed. */
	return DIV_ROUND_UP(bits_per_index, bits_per_page);
}

void log_delta_index_entry(struct delta_index_entry *delta_entry)
{
	uds_log_ratelimit(uds_log_info,
			  "List 0x%X Key 0x%X Offset 0x%X%s%s List_size 0x%X%s",
			  delta_entry->list_number,
			  delta_entry->key,
			  delta_entry->offset,
			  delta_entry->at_end ? " end" : "",
			  delta_entry->is_collision ? " collision" : "",
			  get_delta_list_size(delta_entry->delta_list),
			  delta_entry->list_overflow ? " overflow" : "");
	delta_entry->list_overflow = false;
}
