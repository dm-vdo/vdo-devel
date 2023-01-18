/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_COMPONENT_STATES_H
#define VDO_COMPONENT_STATES_H

#include <linux/limits.h>

#include "numeric.h"

#include "constants.h"
#include "journal-point.h"
#include "slab-depot-format.h"
#include "types.h"
#include "vdo-component.h"
#include "vdo-layout.h"

/**
 * DOC: Block map entries
 *
 * The entry for each logical block in the block map is encoded into five bytes, which saves space
 * in both the on-disk and in-memory layouts. It consists of the 36 low-order bits of a
 * physical_block_number_t (addressing 256 terabytes with a 4KB block size) and a 4-bit encoding of
 * a block_mapping_state.
 *
 * Of the 8 high bits of the 5-byte structure:
 *
 * Bits 7..4: The four highest bits of the 36-bit physical block number
 * Bits 3..0: The 4-bit block_mapping_state
 *
 * The following 4 bytes are the low order bytes of the physical block number, in little-endian
 * order.
 *
 * Conversion functions to and from a data location are provided.
 */
struct block_map_entry {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned mapping_state : 4;
	unsigned pbn_high_nibble : 4;
#else
	unsigned pbn_high_nibble : 4;
	unsigned mapping_state : 4;
#endif

	__le32 pbn_low_word;
} __packed;

struct block_map_state_2_0 {
	physical_block_number_t flat_page_origin;
	block_count_t flat_page_count;
	physical_block_number_t root_origin;
	block_count_t root_count;
} __packed;

struct boundary {
	page_number_t levels[VDO_BLOCK_MAP_TREE_HEIGHT];
};

extern const struct header VDO_BLOCK_MAP_HEADER_2_0;

/* The state of the recovery journal as encoded in the VDO super block. */
struct recovery_journal_state_7_0 {
	/** Sequence number to start the journal */
	sequence_number_t journal_start;
	/** Number of logical blocks used by VDO */
	block_count_t logical_blocks_used;
	/** Number of block map pages allocated */
	block_count_t block_map_data_blocks;
} __packed;

extern const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0;

/*
 * A recovery journal entry stores two physical locations: a data location that is the value of a
 * single mapping in the block map tree, and the location of the block map page and slot that is
 * either acquiring or releasing a reference to the data location. The journal entry also stores an
 * operation code that says whether the reference is being acquired (an increment) or released (a
 * decrement), and whether the mapping is for a logical block or for the block map tree itself.
 */
struct recovery_journal_entry {
	struct block_map_slot slot;
	struct data_location mapping;
	enum journal_operation operation;
};

/* The packed, on-disk representation of a recovery journal entry. */
struct packed_recovery_journal_entry {
	/*
	 * In little-endian bit order:
	 * Bits 15..12: The four highest bits of the 36-bit physical block number of the block map
	 * tree page Bits 11..2: The 10-bit block map page slot number Bits 1..0: The 2-bit
	 * journal_operation of the entry
	 */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	unsigned operation : 2;
	unsigned slot_low : 6;
	unsigned slot_high : 4;
	unsigned pbn_high_nibble : 4;
#else
	unsigned slot_low : 6;
	unsigned operation : 2;
	unsigned pbn_high_nibble : 4;
	unsigned slot_high : 4;
#endif

	/*
	 * Bits 47..16: The 32 low-order bits of the block map page PBN, in little-endian byte
	 * order
	 */
	__le32 pbn_low_word;

	/*
	 * Bits 87..48: The five-byte block map entry encoding the location that was or will be
	 * stored in the block map page slot
	 */
	struct block_map_entry block_map_entry;
} __packed;

struct recovery_block_header {
	sequence_number_t block_map_head; /* Block map head sequence number */
	sequence_number_t slab_journal_head; /* Slab journal head seq. number */
	sequence_number_t sequence_number; /* Sequence number for this block */
	nonce_t nonce; /* A given VDO instance's nonce */
	block_count_t logical_blocks_used; /* Logical blocks in use */
	block_count_t block_map_data_blocks; /* Allocated block map pages */
	journal_entry_count_t entry_count; /* Number of entries written */
	u8 check_byte; /* The protection check byte */
	u8 recovery_count; /* Number of recoveries completed */
	enum vdo_metadata_type metadata_type; /* Metadata type */
};

/*
 * The packed, on-disk representation of a recovery journal block header. All fields are kept in
 * little-endian byte order.
 */
struct packed_journal_header {
	/* Block map head 64-bit sequence number */
	__le64 block_map_head;

	/* Slab journal head 64-bit sequence number */
	__le64 slab_journal_head;

	/* The 64-bit sequence number for this block */
	__le64 sequence_number;

	/* A given VDO instance's 64-bit nonce */
	__le64 nonce;

	/* 8-bit metadata type (should always be one for the recovery journal) */
	u8 metadata_type;

	/* 16-bit count of the entries encoded in the block */
	__le16 entry_count;

	/* 64-bit count of the logical blocks used when this block was opened */
	__le64 logical_blocks_used;

	/* 64-bit count of the block map blocks used when this block was opened */
	__le64 block_map_data_blocks;

	/* The protection check byte */
	u8 check_byte;

	/* The number of recoveries completed */
	u8 recovery_count;
} __packed;

struct packed_journal_sector {
	/* The protection check byte */
	u8 check_byte;

	/* The number of recoveries completed */
	u8 recovery_count;

	/* The number of entries in this sector */
	u8 entry_count;

	/* Journal entries for this sector */
	struct packed_recovery_journal_entry entries[];
} __packed;

enum {
	BLOCK_MAP_COMPONENT_ENCODED_SIZE =
		VDO_ENCODED_HEADER_SIZE + sizeof(struct block_map_state_2_0),
	/*
	 * Allowing more than 311 entries in each block changes the math concerning the
	 * amortization of metadata writes and recovery speed.
	 */
	RECOVERY_JOURNAL_ENTRIES_PER_BLOCK = 311,
	/* The number of entries in each sector (except the last) when filled */
	RECOVERY_JOURNAL_ENTRIES_PER_SECTOR =
		((VDO_SECTOR_SIZE - sizeof(struct packed_journal_sector)) /
		 sizeof(struct packed_recovery_journal_entry)),
	/* The number of entries in the last sector when a block is full */
	RECOVERY_JOURNAL_ENTRIES_PER_LAST_SECTOR =
		(RECOVERY_JOURNAL_ENTRIES_PER_BLOCK %
		 RECOVERY_JOURNAL_ENTRIES_PER_SECTOR),
	RECOVERY_JOURNAL_COMPONENT_ENCODED_SIZE =
		VDO_ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0),
};

/*
 * The version of the on-disk format of a VDO volume. This should be incremented any time the
 * on-disk representation of any VDO structure changes. Changes which require only online upgrade
 * steps should increment the minor version. Changes which require an offline upgrade or which can
 * not be upgraded to at all should increment the major version and set the minor version to 0.
 */
extern const struct version_number VDO_VOLUME_VERSION_67_0;

/* The entirety of the component data encoded in the VDO super block. */
struct vdo_component_states {
	/* The release version */
	release_version_number_t release_version;

	/* The VDO volume version */
	struct version_number volume_version;

	/* Components */
	struct vdo_component vdo;
	struct block_map_state_2_0 block_map;
	struct recovery_journal_state_7_0 recovery_journal;
	struct slab_depot_state_2_0 slab_depot;

	/* Our partitioning of the underlying storage */
	struct fixed_layout *layout;
};

static inline bool vdo_is_state_compressed(const enum block_mapping_state mapping_state)
{
	return (mapping_state > VDO_MAPPING_STATE_UNCOMPRESSED);
}

static inline struct block_map_entry
vdo_pack_block_map_entry(physical_block_number_t pbn, enum block_mapping_state mapping_state)
{
	return (struct block_map_entry) {
		.mapping_state = (mapping_state & 0x0F),
		.pbn_high_nibble = ((pbn >> 32) & 0x0F),
		.pbn_low_word = __cpu_to_le32(pbn & UINT_MAX),
	};
}

static inline struct data_location vdo_unpack_block_map_entry(const struct block_map_entry *entry)
{
	physical_block_number_t low32 = __le32_to_cpu(entry->pbn_low_word);
	physical_block_number_t high4 = entry->pbn_high_nibble;

	return (struct data_location) {
		.pbn = ((high4 << 32) | low32),
		.state = entry->mapping_state,
	};
}

static inline bool vdo_is_mapped_location(const struct data_location *location)
{
	return (location->state != VDO_MAPPING_STATE_UNMAPPED);
}

static inline bool vdo_is_valid_location(const struct data_location *location)
{
	if (location->pbn == VDO_ZERO_BLOCK)
		return !vdo_is_state_compressed(location->state);
	else
		return vdo_is_mapped_location(location);
}

#ifdef INTERNAL
int __must_check
decode_block_map_state_2_0(struct buffer *buffer, struct block_map_state_2_0 *state);
int __must_check
encode_block_map_state_2_0(struct block_map_state_2_0 state, struct buffer *buffer);
#endif /* INTERNAL */
static inline page_count_t vdo_compute_block_map_page_count(block_count_t entries)
{
	return DIV_ROUND_UP(entries, VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
}

block_count_t __must_check
vdo_compute_new_forest_pages(root_count_t root_count,
			     struct boundary *old_sizes,
			     block_count_t entries,
			     struct boundary *new_sizes);

/**
 * vdo_pack_recovery_journal_entry() - Return the packed, on-disk representation of a recovery
 *                                     journal entry.
 * @entry: The journal entry to pack.
 *
 * Return: The packed representation of the journal entry.
 */
static inline struct packed_recovery_journal_entry
vdo_pack_recovery_journal_entry(const struct recovery_journal_entry *entry)
{
	return (struct packed_recovery_journal_entry) {
		.operation = entry->operation,
		.slot_low = entry->slot.slot & 0x3F,
		.slot_high = (entry->slot.slot >> 6) & 0x0F,
		.pbn_high_nibble = (entry->slot.pbn >> 32) & 0x0F,
		.pbn_low_word = __cpu_to_le32(entry->slot.pbn & UINT_MAX),
		.block_map_entry = vdo_pack_block_map_entry(entry->mapping.pbn,
							    entry->mapping.state),
	};
}

/**
 * vdo_unpack_recovery_journal_entry() - Unpack the on-disk representation of a recovery journal
 *                                       entry.
 * @entry: The recovery journal entry to unpack.
 *
 * Return: The unpacked entry.
 */
static inline struct recovery_journal_entry
vdo_unpack_recovery_journal_entry(const struct packed_recovery_journal_entry *entry)
{
	physical_block_number_t low32 = __le32_to_cpu(entry->pbn_low_word);
	physical_block_number_t high4 = entry->pbn_high_nibble;

	return (struct recovery_journal_entry) {
		.operation = entry->operation,
		.slot = {
				.pbn = ((high4 << 32) | low32),
				.slot = (entry->slot_low | (entry->slot_high << 6)),
			},
		.mapping = vdo_unpack_block_map_entry(&entry->block_map_entry),
	};
}

#ifdef INTERNAL
int __must_check
encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state, struct buffer *buffer);
int __must_check
decode_recovery_journal_state_7_0(struct buffer *buffer, struct recovery_journal_state_7_0 *state);
#endif /* INTERNAL */
const char * __must_check vdo_get_journal_operation_name(enum journal_operation operation);

/**
 * vdo_is_valid_recovery_journal_sector() - Determine whether the header of the given sector could
 *                                          describe a valid sector for the given journal block
 *                                          header.
 * @header: The unpacked block header to compare against.
 * @sector: The packed sector to check.
 *
 * Return: true if the sector matches the block header.
 */
static inline bool __must_check
vdo_is_valid_recovery_journal_sector(const struct recovery_block_header *header,
				     const struct packed_journal_sector *sector)
{
	return ((header->check_byte == sector->check_byte) &&
		(header->recovery_count == sector->recovery_count));
}

/**
 * vdo_compute_recovery_journal_block_number() - Compute the physical block number of the recovery
 *                                               journal block which would have a given sequence
 *                                               number.
 * @journal_size: The size of the journal.
 * @sequence_number: The sequence number.
 *
 * Return: The pbn of the journal block which would the specified sequence number.
 */
static inline physical_block_number_t __must_check
vdo_compute_recovery_journal_block_number(block_count_t journal_size,
					  sequence_number_t sequence_number)
{
	/*
	 * Since journal size is a power of two, the block number modulus can just be extracted
	 * from the low-order bits of the sequence.
	 */
	return (sequence_number & (journal_size - 1));
}

/**
 * vdo_get_journal_block_sector() - Find the recovery journal sector from the block header and
 *                                  sector number.
 * @header: The header of the recovery journal block.
 * @sector_number: The index of the sector (1-based).
 *
 * Return: A packed recovery journal sector.
 */
static inline struct packed_journal_sector * __must_check
vdo_get_journal_block_sector(struct packed_journal_header *header, int sector_number)
{
	char *sector_data = ((char *) header) + (VDO_SECTOR_SIZE * sector_number);

	return (struct packed_journal_sector *) sector_data;
}

/**
 * vdo_pack_recovery_block_header() - Generate the packed representation of a recovery block
 *                                    header.
 * @header: The header containing the values to encode.
 * @packed: The header into which to pack the values.
 */
static inline void vdo_pack_recovery_block_header(const struct recovery_block_header *header,
						  struct packed_journal_header *packed)
{
	*packed = (struct packed_journal_header) {
		.block_map_head = __cpu_to_le64(header->block_map_head),
		.slab_journal_head = __cpu_to_le64(header->slab_journal_head),
		.sequence_number = __cpu_to_le64(header->sequence_number),
		.nonce = __cpu_to_le64(header->nonce),
		.logical_blocks_used = __cpu_to_le64(header->logical_blocks_used),
		.block_map_data_blocks = __cpu_to_le64(header->block_map_data_blocks),
		.entry_count = __cpu_to_le16(header->entry_count),
		.check_byte = header->check_byte,
		.recovery_count = header->recovery_count,
		.metadata_type = header->metadata_type,
	};
}

/**
 * vdo_unpack_recovery_block_header() - Decode the packed representation of a recovery block
 *                                      header.
 * @packed: The packed header to decode.
 * @header: The header into which to unpack the values.
 */
static inline void vdo_unpack_recovery_block_header(const struct packed_journal_header *packed,
						    struct recovery_block_header *header)
{
	*header = (struct recovery_block_header) {
		.block_map_head = __le64_to_cpu(packed->block_map_head),
		.slab_journal_head = __le64_to_cpu(packed->slab_journal_head),
		.sequence_number = __le64_to_cpu(packed->sequence_number),
		.nonce = __le64_to_cpu(packed->nonce),
		.logical_blocks_used = __le64_to_cpu(packed->logical_blocks_used),
		.block_map_data_blocks = __le64_to_cpu(packed->block_map_data_blocks),
		.entry_count = __le16_to_cpu(packed->entry_count),
		.check_byte = packed->check_byte,
		.recovery_count = packed->recovery_count,
		.metadata_type = packed->metadata_type,
	};
}

void vdo_destroy_component_states(struct vdo_component_states *states);

int __must_check
vdo_decode_component_states(struct buffer *buffer,
			    release_version_number_t expected_release_version,
			    struct vdo_component_states *states);

int __must_check
vdo_validate_component_states(struct vdo_component_states *states,
			      nonce_t geometry_nonce,
			      block_count_t physical_size,
			      block_count_t logical_size);

/**
 * vdo_encode() - Encode a VDO super block into a buffer for writing in the super block.
 * @buffer: The buffer to encode into.
 * @states: The states of the vdo to be encoded.
 */
int __must_check vdo_encode(struct buffer *buffer, struct vdo_component_states *states);

int vdo_encode_component_states(struct buffer *buffer, const struct vdo_component_states *states);

#endif /* VDO_COMPONENT_STATES_H */
