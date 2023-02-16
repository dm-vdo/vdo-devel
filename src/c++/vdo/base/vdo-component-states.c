// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-component-states.h"

#include <linux/log2.h>

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "constants.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-layout.h"

enum {
	PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const struct version_number BLOCK_MAP_4_1 = {
	.major_version = 4,
	.minor_version = 1,
};

const struct header VDO_BLOCK_MAP_HEADER_2_0 = {
	.id = VDO_BLOCK_MAP,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct block_map_state_2_0),
};

const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = VDO_RECOVERY_JOURNAL,
	.version = {
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

const struct header VDO_SLAB_DEPOT_HEADER_2_0 = {
	.id = VDO_SLAB_DEPOT,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct slab_depot_state_2_0),
};

/*
 * The current version for the data encoded in the super block. This must be changed any time there
 * is a change to encoding of the component data of any VDO component.
 */
static const struct version_number VDO_COMPONENT_DATA_41_0 = {
	.major_version = 41,
	.minor_version = 0,
};

const struct version_number VDO_VOLUME_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

static const struct header SUPER_BLOCK_HEADER_12_0 = {
	.id = VDO_SUPER_BLOCK,
	.version = {
			.major_version = 12,
			.minor_version = 0,
		},

	/* This is the minimum size, if the super block contains no components. */
	.size = VDO_SUPER_BLOCK_FIXED_SIZE - VDO_ENCODED_HEADER_SIZE,
};

/**
 * validate_version() - Check whether a version matches an expected version.
 * @expected_version: The expected version.
 * @actual_version: The version being validated.
 * @component_name: The name of the component or the calling function (for error logging).
 *
 * Logs an error describing a mismatch.
 *
 * Return: VDO_SUCCESS             if the versions are the same,
 *         VDO_UNSUPPORTED_VERSION if the versions don't match.
 */
static int __must_check validate_version(struct version_number expected_version,
					 struct version_number actual_version,
					 const char *component_name)
{
	if (!vdo_are_same_version(expected_version, actual_version))
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "%s version mismatch, expected %d.%d, got %d.%d",
					      component_name,
					      expected_version.major_version,
					      expected_version.minor_version,
					      actual_version.major_version,
					      actual_version.minor_version);
	return VDO_SUCCESS;
}

/**
 * vdo_validate_header() - Check whether a header matches expectations.
 * @expected_header: The expected header.
 * @actual_header: The header being validated.
 * @exact_size: If true, the size fields of the two headers must be the same, otherwise it is
 *              required that actual_header.size >= expected_header.size.
 * @name: The name of the component or the calling function (for error logging).
 *
 * Logs an error describing the first mismatch found.
 *
 * Return: VDO_SUCCESS             if the header meets expectations,
 *         VDO_INCORRECT_COMPONENT if the component ids don't match,
 *         VDO_UNSUPPORTED_VERSION if the versions or sizes don't match.
 */
int vdo_validate_header(const struct header *expected_header,
			const struct header *actual_header,
			bool exact_size,
			const char *name)
{
	int result;

	if (expected_header->id != actual_header->id)
		return uds_log_error_strerror(VDO_INCORRECT_COMPONENT,
					      "%s ID mismatch, expected %d, got %d",
					      name,
					      expected_header->id,
					      actual_header->id);

	result = validate_version(expected_header->version, actual_header->version, name);
	if (result != VDO_SUCCESS)
		return result;

	if ((expected_header->size > actual_header->size) ||
	    (exact_size && (expected_header->size < actual_header->size)))
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "%s size mismatch, expected %zu, got %zu",
					      name,
					      expected_header->size,
					      actual_header->size);

	return VDO_SUCCESS;
}

/**
 * encode_version_number() - Encode a version number into a buffer.
 * @version: The version to encode.
 * @buffer: The buffer in which to encode the version.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int encode_version_number(struct version_number version, struct buffer *buffer)
{
	struct packed_version_number packed = vdo_pack_version_number(version);

	return put_bytes(buffer, sizeof(packed), &packed);
}

/**
 * vdo_encode_header() - Encode a header into a buffer.
 * @header: The header to encode.
 * @buffer: The buffer in which to encode the header.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_encode_header(const struct header *header, struct buffer *buffer)
{
	int result;

	if (!ensure_available_space(buffer, VDO_ENCODED_HEADER_SIZE))
		return UDS_BUFFER_ERROR;

	result = put_u32_le_into_buffer(buffer, header->id);
	if (result != UDS_SUCCESS)
		return result;

	result = encode_version_number(header->version, buffer);
	if (result != UDS_SUCCESS)
		return result;

	return put_u64_le_into_buffer(buffer, header->size);
}

/**
 * decode_version_number() - Decode a version number from a buffer.
 * @buffer: The buffer from which to decode the version.
 * @version: The version structure to decode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
static int decode_version_number(struct buffer *buffer, struct version_number *version)
{
	struct packed_version_number packed;
	int result;

	result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);
	if (result != UDS_SUCCESS)
		return result;

	*version = vdo_unpack_version_number(packed);
	return UDS_SUCCESS;
}

/**
 * vdo_decode_header() - Decode a header from a buffer.
 * @buffer: The buffer from which to decode the header.
 * @header: The header structure to decode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
int vdo_decode_header(struct buffer *buffer, struct header *header)
{
	u32 id;
	u64 size;
	struct version_number version;
	int result;

	result = get_u32_le_from_buffer(buffer, &id);

	if (result != UDS_SUCCESS)
		return result;

	result = decode_version_number(buffer, &version);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &size);
	if (result != UDS_SUCCESS)
		return result;

	*header = (struct header) {
		.id = id,
		.version = version,
		.size = size,
	};
	return UDS_SUCCESS;
}

struct block_map_page *vdo_format_block_map_page(void *buffer,
						 nonce_t nonce,
						 physical_block_number_t pbn,
						 bool initialized)
{
	struct block_map_page *page = (struct block_map_page *) buffer;

	memset(buffer, 0, VDO_BLOCK_SIZE);
	page->version = vdo_pack_version_number(BLOCK_MAP_4_1);
	page->header.nonce = __cpu_to_le64(nonce);
	page->header.pbn = __cpu_to_le64(pbn);
	page->header.initialized = initialized;
	return page;
}

enum block_map_page_validity
vdo_validate_block_map_page(struct block_map_page *page,
			    nonce_t nonce,
			    physical_block_number_t pbn)
{
	STATIC_ASSERT_SIZEOF(struct block_map_page_header, PAGE_HEADER_4_1_SIZE);

	if (!vdo_are_same_version(BLOCK_MAP_4_1, vdo_unpack_version_number(page->version)) ||
	    !page->header.initialized ||
	    (nonce != __le64_to_cpu(page->header.nonce)))
		return VDO_BLOCK_MAP_PAGE_INVALID;

	if (pbn != vdo_get_block_map_page_pbn(page))
		return VDO_BLOCK_MAP_PAGE_BAD;

	return VDO_BLOCK_MAP_PAGE_VALID;
}

EXTERNAL_STATIC int
decode_block_map_state_2_0(struct buffer *buffer, struct block_map_state_2_0 *state)
{
	size_t initial_length, decoded_size;
	block_count_t flat_page_count, root_count;
	physical_block_number_t flat_page_origin, root_origin;
	struct header header;
	int result;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&VDO_BLOCK_MAP_HEADER_2_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = get_u64_le_from_buffer(buffer, &flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(flat_page_origin == VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			"Flat page origin must be %u (recorded as %llu)",
			VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN,
			(unsigned long long) state->flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(flat_page_count == 0,
			"Flat page count must be 0 (recorded as %llu)",
			(unsigned long long) state->flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &root_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &root_count);
	if (result != UDS_SUCCESS)
		return result;

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == decoded_size,
			"decoded block map component size must match header size");
	if (result != VDO_SUCCESS)
		return result;

	*state = (struct block_map_state_2_0) {
		.flat_page_origin = flat_page_origin,
		.flat_page_count = flat_page_count,
		.root_origin = root_origin,
		.root_count = root_count,
	};

	return VDO_SUCCESS;
}

EXTERNAL_STATIC int
encode_block_map_state_2_0(struct block_map_state_2_0 state, struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	result = vdo_encode_header(&VDO_BLOCK_MAP_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = put_u64_le_into_buffer(buffer, state.flat_page_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.flat_page_count);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.root_origin);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.root_count);
	if (result != UDS_SUCCESS)
		return result;

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_BLOCK_MAP_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * vdo_compute_new_forest_pages() - Compute the number of pages which must be allocated at each
 *                                  level in order to grow the forest to a new number of entries.
 * @entries: The new number of entries the block map must address.
 *
 * Return: The total number of non-leaf pages required.
 */
block_count_t vdo_compute_new_forest_pages(root_count_t root_count,
					   struct boundary *old_sizes,
					   block_count_t entries,
					   struct boundary *new_sizes)
{
	page_count_t leaf_pages = max(vdo_compute_block_map_page_count(entries), 1U);
	page_count_t level_size = DIV_ROUND_UP(leaf_pages, root_count);
	block_count_t total_pages = 0;
	height_t height;

	for (height = 0; height < VDO_BLOCK_MAP_TREE_HEIGHT; height++) {
		block_count_t new_pages;

		level_size = DIV_ROUND_UP(level_size, VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
		new_sizes->levels[height] = level_size;
		new_pages = level_size;
		if (old_sizes != NULL)
			new_pages -= old_sizes->levels[height];
		total_pages += (new_pages * root_count);
	}

	return total_pages;
}

/**
 * encode_recovery_journal_state_7_0() - Encode the state of a recovery journal.
 * @state: The recovery journal state.
 * @buffer: The buffer to encode into.
 *
 * Return: VDO_SUCCESS or an error code.
 */
EXTERNAL_STATIC int __must_check
encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state, struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	result = vdo_encode_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, buffer);
	if (result != UDS_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = put_u64_le_into_buffer(buffer, state.journal_start);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.logical_blocks_used);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.block_map_data_blocks);
	if (result != UDS_SUCCESS)
		return result;

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == encoded_size,
		      "encoded recovery journal component size must match header size");
}

/**
 * decode_recovery_journal_state_7_0() - Decode the state of a recovery journal saved in a buffer.
 * @buffer: The buffer containing the saved state.
 * @state: A pointer to a recovery journal state to hold the result of a successful decode.
 *
 * Return: VDO_SUCCESS or an error code.
 */
EXTERNAL_STATIC int __must_check
decode_recovery_journal_state_7_0(struct buffer *buffer, struct recovery_journal_state_7_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	sequence_number_t journal_start;
	block_count_t logical_blocks_used, block_map_data_blocks;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&VDO_RECOVERY_JOURNAL_HEADER_7_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = get_u64_le_from_buffer(buffer, &journal_start);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &logical_blocks_used);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &block_map_data_blocks);
	if (result != UDS_SUCCESS)
		return result;

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_RECOVERY_JOURNAL_HEADER_7_0.size == decoded_size,
			"decoded recovery journal component size must match header size");
	if (result != UDS_SUCCESS)
		return result;

	*state = (struct recovery_journal_state_7_0) {
		.journal_start = journal_start,
		.logical_blocks_used = logical_blocks_used,
		.block_map_data_blocks = block_map_data_blocks,
	};

	return VDO_SUCCESS;
}

/**
 * vdo_get_journal_operation_name() - Get the name of a journal operation.
 * @operation: The operation to name.
 *
 * Return: The name of the operation.
 */
const char *vdo_get_journal_operation_name(enum journal_operation operation)
{
	switch (operation) {
	case VDO_JOURNAL_DATA_REMAPPING:
		return "data remapping";

	case VDO_JOURNAL_BLOCK_MAP_REMAPPING:
		return "block map remapping";

	default:
		return "unknown journal operation";
	}
}

/**
 * encode_slab_config() - Encode a slab config into a buffer.
 * @config: The config structure to encode.
 * @buffer: A buffer positioned at the start of the encoding.
 *
 * Return: UDS_SUCCESS or an error code.
 */
static int encode_slab_config(const struct slab_config *config, struct buffer *buffer)
{
	int result;

	result = put_u64_le_into_buffer(buffer, config->slab_blocks);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, config->data_blocks);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, config->reference_count_blocks);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, config->slab_journal_blocks);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, config->slab_journal_flushing_threshold);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, config->slab_journal_blocking_threshold);
	if (result != UDS_SUCCESS)
		return result;

	return put_u64_le_into_buffer(buffer, config->slab_journal_scrubbing_threshold);
}

/**
 * encode_slab_depot_state_2_0() - Encode the state of a slab depot into a buffer.
 * @state: The state to encode.
 * @buffer: The buffer to encode into.
 *
 * Return: UDS_SUCCESS or an error.
 */
EXTERNAL_STATIC int
encode_slab_depot_state_2_0(struct slab_depot_state_2_0 state, struct buffer *buffer)
{
	size_t initial_length, encoded_size;
	int result;

	result = vdo_encode_header(&VDO_SLAB_DEPOT_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = encode_slab_config(&state.slab_config, buffer);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.first_block);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u64_le_into_buffer(buffer, state.last_block);
	if (result != UDS_SUCCESS)
		return result;

	result = put_byte(buffer, state.zone_count);
	if (result != UDS_SUCCESS)
		return result;

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_SLAB_DEPOT_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * decode_slab_config() - Decode a slab config from a buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @config: The config structure to receive the decoded values.
 *
 * Return: UDS_SUCCESS or an error code.
 */
static int decode_slab_config(struct buffer *buffer, struct slab_config *config)
{
	block_count_t count;
	int result;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->slab_blocks = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->data_blocks = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->reference_count_blocks = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->slab_journal_blocks = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->slab_journal_flushing_threshold = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->slab_journal_blocking_threshold = count;

	result = get_u64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS)
		return result;
	config->slab_journal_scrubbing_threshold = count;

	return UDS_SUCCESS;
}

/**
 * decode_slab_depot_state_2_0() - Decode slab depot component state version 2.0 from a buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @state: The state structure to receive the decoded values.
 *
 * Return: UDS_SUCCESS or an error code.
 */
EXTERNAL_STATIC int
decode_slab_depot_state_2_0(struct buffer *buffer, struct slab_depot_state_2_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	struct slab_config slab_config;
	physical_block_number_t first_block, last_block;
	zone_count_t zone_count;

	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&VDO_SLAB_DEPOT_HEADER_2_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	initial_length = content_length(buffer);

	result = decode_slab_config(buffer, &slab_config);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &first_block);
	if (result != UDS_SUCCESS)
		return result;

	result = get_u64_le_from_buffer(buffer, &last_block);
	if (result != UDS_SUCCESS)
		return result;

	result = get_byte(buffer, &zone_count);
	if (result != UDS_SUCCESS)
		return result;

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_SLAB_DEPOT_HEADER_2_0.size == decoded_size,
			"decoded slab depot component size must match header size");
	if (result != UDS_SUCCESS)
		return result;

	*state = (struct slab_depot_state_2_0) {
		.slab_config = slab_config,
		.first_block = first_block,
		.last_block = last_block,
		.zone_count = zone_count,
	};

	return VDO_SUCCESS;
}

/**
 * vdo_configure_slab_depot() - Configure the slab depot.
 * @block_count: The number of blocks in the underlying storage.
 * @first_block: The number of the first block that may be allocated.
 * @slab_config: The configuration of a single slab.
 * @zone_count: The number of zones the depot will use.
 * @state: The state structure to be configured.
 *
 * Configures the slab_depot for the specified storage capacity, finding the number of data blocks
 * that will fit and still leave room for the depot metadata, then return the saved state for that
 * configuration.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_configure_slab_depot(block_count_t block_count,
			     physical_block_number_t first_block,
			     struct slab_config slab_config,
			     zone_count_t zone_count,
			     struct slab_depot_state_2_0 *state)
{
	block_count_t total_slab_blocks, total_data_blocks;
	size_t slab_count;
	physical_block_number_t last_block;
	block_count_t slab_size = slab_config.slab_blocks;

	uds_log_debug("slabDepot %s(block_count=%llu, first_block=%llu, slab_size=%llu, zone_count=%u)",
		      __func__,
		      (unsigned long long) block_count,
		      (unsigned long long) first_block,
		      (unsigned long long) slab_size,
		      zone_count);

	/* We do not allow runt slabs, so we waste up to a slab's worth. */
	slab_count = (block_count / slab_size);
	if (slab_count == 0)
		return VDO_NO_SPACE;

	if (slab_count > MAX_VDO_SLABS)
		return VDO_TOO_MANY_SLABS;

	total_slab_blocks = slab_count * slab_config.slab_blocks;
	total_data_blocks = slab_count * slab_config.data_blocks;
	last_block = first_block + total_slab_blocks;

	*state = (struct slab_depot_state_2_0) {
		.slab_config = slab_config,
		.first_block = first_block,
		.last_block = last_block,
		.zone_count = zone_count,
	};

	uds_log_debug("slab_depot last_block=%llu, total_data_blocks=%llu, slab_count=%zu, left_over=%llu",
		      (unsigned long long) last_block,
		      (unsigned long long) total_data_blocks,
		      slab_count,
		      (unsigned long long) (block_count - (last_block - first_block)));

	return VDO_SUCCESS;
}

/**
 * vdo_configure_slab() - Measure and initialize the configuration to use for each slab.
 * @slab_size: The number of blocks per slab.
 * @slab_journal_blocks: The number of blocks for the slab journal.
 * @slab_config: The slab configuration to initialize.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_configure_slab(block_count_t slab_size,
		       block_count_t slab_journal_blocks,
		       struct slab_config *slab_config)
{
	block_count_t ref_blocks, meta_blocks, data_blocks;
	block_count_t flushing_threshold, remaining, blocking_threshold;
	block_count_t minimal_extra_space, scrubbing_threshold;

	if (slab_journal_blocks >= slab_size)
		return VDO_BAD_CONFIGURATION;

	/*
	 * This calculation should technically be a recurrence, but the total number of metadata
	 * blocks is currently less than a single block of ref_counts, so we'd gain at most one
	 * data block in each slab with more iteration.
	 */
	ref_blocks = vdo_get_saved_reference_count_size(slab_size - slab_journal_blocks);
	meta_blocks = (ref_blocks + slab_journal_blocks);

	/* Make sure test code hasn't configured slabs to be too small. */
	if (meta_blocks >= slab_size)
		return VDO_BAD_CONFIGURATION;

	/*
	 * If the slab size is very small, assume this must be a unit test and override the number
	 * of data blocks to be a power of two (wasting blocks in the slab). Many tests need their
	 * data_blocks fields to be the exact capacity of the configured volume, and that used to
	 * fall out since they use a power of two for the number of data blocks, the slab size was
	 * a power of two, and every block in a slab was a data block.
	 *
	 * TODO: Try to figure out some way of structuring testParameters and unit tests so this
	 * hack isn't needed without having to edit several unit tests every time the metadata size
	 * changes by one block.
	 */
	data_blocks = slab_size - meta_blocks;
	if ((slab_size < 1024) && !is_power_of_2(data_blocks))
		data_blocks = ((block_count_t) 1 << ilog2(data_blocks));

	/*
	 * Configure the slab journal thresholds. The flush threshold is 168 of 224 blocks in
	 * production, or 3/4ths, so we use this ratio for all sizes.
	 */
	flushing_threshold = ((slab_journal_blocks * 3) + 3) / 4;
	/*
	 * The blocking threshold should be far enough from the flushing threshold to not produce
	 * delays, but far enough from the end of the journal to allow multiple successive recovery
	 * failures.
	 */
	remaining = slab_journal_blocks - flushing_threshold;
	blocking_threshold = flushing_threshold + ((remaining * 5) / 7);
	/* The scrubbing threshold should be at least 2048 entries before the end of the journal. */
	minimal_extra_space = 1 + (MAXIMUM_VDO_USER_VIOS / VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK);
	scrubbing_threshold = blocking_threshold;
	if (slab_journal_blocks > minimal_extra_space)
		scrubbing_threshold = slab_journal_blocks - minimal_extra_space;
	if (blocking_threshold > scrubbing_threshold)
		blocking_threshold = scrubbing_threshold;

	*slab_config = (struct slab_config) {
		.slab_blocks = slab_size,
		.data_blocks = data_blocks,
		.reference_count_blocks = ref_blocks,
		.slab_journal_blocks = slab_journal_blocks,
		.slab_journal_flushing_threshold = flushing_threshold,
		.slab_journal_blocking_threshold = blocking_threshold,
		.slab_journal_scrubbing_threshold = scrubbing_threshold};
	return VDO_SUCCESS;
}

/**
 * vdo_decode_slab_journal_entry() - Decode a slab journal entry.
 * @block: The journal block holding the entry.
 * @entry_count: The number of the entry.
 *
 * Return: The decoded entry.
 */
struct slab_journal_entry
vdo_decode_slab_journal_entry(struct packed_slab_journal_block *block,
			      journal_entry_count_t entry_count)
{
	struct slab_journal_entry entry =
		vdo_unpack_slab_journal_entry(&block->payload.entries[entry_count]);

	if (block->header.has_block_map_increments &&
	    ((block->payload.full_entries.entry_types[entry_count / 8] &
	      ((u8)1 << (entry_count % 8))) != 0))
		entry.operation = VDO_JOURNAL_BLOCK_MAP_REMAPPING;

	return entry;
}

/**
 * pack_vdo_config() - Convert a vdo_config to its packed on-disk representation.
 * @config: The vdo config to convert.
 *
 * Return: The platform-independent representation of the config.
 */
static struct packed_vdo_config pack_vdo_config(struct vdo_config config)
{
	return (struct packed_vdo_config) {
		.logical_blocks = __cpu_to_le64(config.logical_blocks),
		.physical_blocks = __cpu_to_le64(config.physical_blocks),
		.slab_size = __cpu_to_le64(config.slab_size),
		.recovery_journal_size = __cpu_to_le64(config.recovery_journal_size),
		.slab_journal_blocks = __cpu_to_le64(config.slab_journal_blocks),
	};
}

/**
 * pack_vdo_component() - Convert a vdo_component to its packed on-disk representation.
 * @component: The VDO component data to convert.
 *
 * Return: The platform-independent representation of the component.
 */
static struct packed_vdo_component_41_0
pack_vdo_component(const struct vdo_component component)
{
	return (struct packed_vdo_component_41_0) {
		.state = __cpu_to_le32(component.state),
		.complete_recoveries = __cpu_to_le64(component.complete_recoveries),
		.read_only_recoveries = __cpu_to_le64(component.read_only_recoveries),
		.config = pack_vdo_config(component.config),
		.nonce = __cpu_to_le64(component.nonce),
	};
}

/**
 * encode_vdo_component() - Encode the component data for the vdo itself.
 * @component: The component structure.
 * @buffer: The buffer in which to encode the vdo.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int encode_vdo_component(struct vdo_component component, struct buffer *buffer)
{
	int result;
	struct packed_vdo_component_41_0 packed;

	result = encode_version_number(VDO_COMPONENT_DATA_41_0, buffer);
	if (result != VDO_SUCCESS)
		return result;

	packed = pack_vdo_component(component);
	return put_bytes(buffer, sizeof(packed), &packed);
}

/**
 * unpack_vdo_config() - Convert a packed_vdo_config to its native in-memory representation.
 * @config: The packed vdo config to convert.
 *
 * Return: The native in-memory representation of the vdo config.
 */
static struct vdo_config unpack_vdo_config(struct packed_vdo_config config)
{
	return (struct vdo_config) {
		.logical_blocks = __le64_to_cpu(config.logical_blocks),
		.physical_blocks = __le64_to_cpu(config.physical_blocks),
		.slab_size = __le64_to_cpu(config.slab_size),
		.recovery_journal_size = __le64_to_cpu(config.recovery_journal_size),
		.slab_journal_blocks = __le64_to_cpu(config.slab_journal_blocks),
	};
}

/**
 * unpack_vdo_component_41_0() - Convert a packed_vdo_component_41_0 to its native in-memory
 *				 representation.
 * @component: The packed vdo component data to convert.
 *
 * Return: The native in-memory representation of the component.
 */
static struct vdo_component
unpack_vdo_component_41_0(struct packed_vdo_component_41_0 component)
{
	return (struct vdo_component) {
		.state = __le32_to_cpu(component.state),
		.complete_recoveries = __le64_to_cpu(component.complete_recoveries),
		.read_only_recoveries = __le64_to_cpu(component.read_only_recoveries),
		.config = unpack_vdo_config(component.config),
		.nonce = __le64_to_cpu(component.nonce),
	};
}

/**
 * vdo_decode_component_41_0() - Decode the version 41.0 component data for the vdo itself from a
 *				 buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @component: The component structure to receive the decoded values.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
decode_vdo_component_41_0(struct buffer *buffer, struct vdo_component *component)
{
	struct packed_vdo_component_41_0 packed;
	int result;

	result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);
	if (result != UDS_SUCCESS)
		return result;

	*component = unpack_vdo_component_41_0(packed);
	return VDO_SUCCESS;
}

/**
 * vdo_decode_component() - Decode the component data for the vdo itself from the component data
 *			    buffer in the super block.
 * @buffer: The buffer being decoded.
 * @component: The component structure in which to store the result of a successful decode.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int decode_vdo_component(struct buffer *buffer, struct vdo_component *component)
{
	struct version_number version;
	int result;

	result = decode_version_number(buffer, &version);
	if (result != VDO_SUCCESS)
		return result;

	result = validate_version(version, VDO_COMPONENT_DATA_41_0, "VDO component data");
	if (result != VDO_SUCCESS)
		return result;

	return decode_vdo_component_41_0(buffer, component);
}

/**
 * vdo_validate_config() - Validate constraints on a VDO config.
 * @config: The VDO config.
 * @physical_block_count: The minimum block count of the underlying storage.
 * @logical_block_count: The expected logical size of the VDO, or 0 if the logical size may be
 *			 unspecified.
 *
 * Return: A success or error code.
 */
int vdo_validate_config(const struct vdo_config *config,
			block_count_t physical_block_count,
			block_count_t logical_block_count)
{
	struct slab_config slab_config;
	int result;

	result = ASSERT(config->slab_size > 0, "slab size unspecified");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(is_power_of_2(config->slab_size), "slab size must be a power of two");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(config->slab_size <= (1 << MAX_VDO_SLAB_BITS),
			"slab size must be less than or equal to 2^%d",
			MAX_VDO_SLAB_BITS);
	if (result != VDO_SUCCESS)
		return result;

	result = ASSERT(config->slab_journal_blocks >= MINIMUM_VDO_SLAB_JOURNAL_BLOCKS,
			"slab journal size meets minimum size");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(config->slab_journal_blocks <= config->slab_size,
			"slab journal size is within expected bound");
	if (result != UDS_SUCCESS)
		return result;

	result = vdo_configure_slab(config->slab_size, config->slab_journal_blocks, &slab_config);
	if (result != VDO_SUCCESS)
		return result;

	result = ASSERT((slab_config.data_blocks >= 1),
			"slab must be able to hold at least one block");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(config->physical_blocks > 0, "physical blocks unspecified");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(config->physical_blocks <= MAXIMUM_VDO_PHYSICAL_BLOCKS,
			"physical block count %llu exceeds maximum %llu",
			(unsigned long long) config->physical_blocks,
			(unsigned long long) MAXIMUM_VDO_PHYSICAL_BLOCKS);
	if (result != UDS_SUCCESS)
		return VDO_OUT_OF_RANGE;

#ifdef __KERNEL__
	if (physical_block_count != config->physical_blocks) {
		uds_log_error("A physical size of %llu blocks was specified, not the %llu blocks configured in the vdo super block",
			      (unsigned long long) physical_block_count,
			      (unsigned long long) config->physical_blocks);
		return VDO_PARAMETER_MISMATCH;
	}
#else
	/*
	 * This can't check equality because FileLayer et al can only known about the storage size,
	 * which may not match the super block size.
	 */
	if (physical_block_count < config->physical_blocks) {
		uds_log_error("A physical size of %llu blocks was specified, but that is smaller than the %llu blocks configured in the vdo super block",
			      (unsigned long long) physical_block_count,
			      (unsigned long long) config->physical_blocks);
		return VDO_PARAMETER_MISMATCH;
	}
#endif /* __KERNEL__ */

	if (logical_block_count > 0) {
		result = ASSERT((config->logical_blocks > 0), "logical blocks unspecified");
		if (result != UDS_SUCCESS)
			return result;

		if (logical_block_count != config->logical_blocks) {
			uds_log_error("A logical size of %llu blocks was specified, but that differs from the %llu blocks configured in the vdo super block",
				      (unsigned long long) logical_block_count,
				      (unsigned long long) config->logical_blocks);
			return VDO_PARAMETER_MISMATCH;
		}
	}

	result = ASSERT(config->logical_blocks <= MAXIMUM_VDO_LOGICAL_BLOCKS,
			"logical blocks too large");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(config->recovery_journal_size > 0, "recovery journal size unspecified");
	if (result != UDS_SUCCESS)
		return result;

	result = ASSERT(is_power_of_2(config->recovery_journal_size),
			"recovery journal size must be a power of two");
	if (result != UDS_SUCCESS)
		return result;

	return result;
}

/**
 * vdo_destroy_component_states() - Clean up any allocations in a vdo_component_states.
 * @states: The component states to destroy.
 */
void vdo_destroy_component_states(struct vdo_component_states *states)
{
	if (states == NULL)
		return;

	vdo_free_fixed_layout(UDS_FORGET(states->layout));
}

/**
 * decode_components() - Decode the components now that we know the component data is a version we
 *                       understand.
 * @buffer: The buffer being decoded.
 * @states: An object to hold the successfully decoded state.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
decode_components(struct buffer *buffer, struct vdo_component_states *states)
{
	int result;

	result = decode_vdo_component(buffer, &states->vdo);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_fixed_layout(buffer, &states->layout);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_recovery_journal_state_7_0(buffer, &states->recovery_journal);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_slab_depot_state_2_0(buffer, &states->slab_depot);
	if (result != VDO_SUCCESS)
		return result;

	result = decode_block_map_state_2_0(buffer, &states->block_map);
	if (result != VDO_SUCCESS)
		return result;

	ASSERT_LOG_ONLY((content_length(buffer) == 0), "All decoded component data was used");
	return VDO_SUCCESS;
}

/**
 * vdo_decode_component_states() - Decode the payload of a super block.
 * @buffer: The buffer containing the encoded super block contents.
 * @expected_release_version: The required release version.
 * @states: A pointer to hold the decoded states.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_component_states(struct buffer *buffer,
				release_version_number_t expected_release_version,
				struct vdo_component_states *states)
{
	int result;

	/* Get and check the release version against the one from the geometry. */
	result = get_u32_le_from_buffer(buffer, &states->release_version);
	if (result != VDO_SUCCESS)
		return result;

	if (states->release_version != expected_release_version)
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "Geometry release version %u does not match super block release version %u",
					      expected_release_version,
					      states->release_version);

	/* Check the VDO volume version */
	result = decode_version_number(buffer, &states->volume_version);
	if (result != VDO_SUCCESS)
		return result;

	result = validate_version(VDO_VOLUME_VERSION_67_0, states->volume_version, "volume");
	if (result != VDO_SUCCESS)
		return result;

	result = decode_components(buffer, states);
	if (result != VDO_SUCCESS) {
		vdo_free_fixed_layout(UDS_FORGET(states->layout));
		return result;
	}

	return VDO_SUCCESS;
}

/**
 * vdo_validate_component_states() - Validate the decoded super block configuration.
 * @states: The state decoded from the super block.
 * @geometry_nonce: The nonce from the geometry block.
 * @physical_size: The minimum block count of the underlying storage.
 * @logical_size: The expected logical size of the VDO, or 0 if the logical size may be
 *                unspecified.
 *
 * Return: VDO_SUCCESS or an error if the configuration is invalid.
 */
int vdo_validate_component_states(struct vdo_component_states *states,
				  nonce_t geometry_nonce,
				  block_count_t physical_size,
				  block_count_t logical_size)
{
	if (geometry_nonce != states->vdo.nonce)
		return uds_log_error_strerror(VDO_BAD_NONCE,
					      "Geometry nonce %llu does not match superblock nonce %llu",
					      (unsigned long long) geometry_nonce,
					      (unsigned long long) states->vdo.nonce);

	return vdo_validate_config(&states->vdo.config, physical_size, logical_size);
}

/**
 * get_component_data_size() - Get the component data size of a vdo.
 * @layout: The layout of the vdo.
 *
 * Return: The component data size of the vdo.
 */
static size_t __must_check get_component_data_size(struct fixed_layout *layout)
{
	return (sizeof(release_version_number_t) +
		sizeof(struct packed_version_number) +
		VDO_COMPONENT_ENCODED_SIZE +
		vdo_get_fixed_layout_encoded_size(layout) +
		RECOVERY_JOURNAL_COMPONENT_ENCODED_SIZE +
		SLAB_DEPOT_COMPONENT_ENCODED_SIZE +
		BLOCK_MAP_COMPONENT_ENCODED_SIZE);
}

/**
 * vdo_encode_component_states() - Encode the state of all vdo components for writing in the super
 *                                 block.
 * @buffer: The buffer to encode into.
 * @states: The states to encode.
 */
int vdo_encode_component_states(struct buffer *buffer, const struct vdo_component_states *states)
{
	size_t expected_size;
	int result;

	result = reset_buffer_end(buffer, 0);
	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, states->release_version);
	if (result != UDS_SUCCESS)
		return result;

	result = encode_version_number(states->volume_version, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_vdo_component(states->vdo, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_fixed_layout(states->layout, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_recovery_journal_state_7_0(states->recovery_journal, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_slab_depot_state_2_0(states->slab_depot, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = encode_block_map_state_2_0(states->block_map, buffer);
	if (result != VDO_SUCCESS)
		return result;

	expected_size = get_component_data_size(states->layout);
	ASSERT_LOG_ONLY((content_length(buffer) == expected_size),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}

/**
 * vdo_initialize_super_block_codec() - Initialize a super block codec.
 * @codec: The codec to initialize.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_initialize_super_block_codec(struct super_block_codec *codec)
{
	int result;

	result = make_buffer(VDO_MAX_COMPONENT_DATA_SIZE, &codec->component_buffer);
	if (result != UDS_SUCCESS)
		return result;

	result = UDS_ALLOCATE(VDO_BLOCK_SIZE,
			      char,
			      "encoded super block",
			      (char **) &codec->encoded_super_block);
	if (result != UDS_SUCCESS)
		return result;

	/*
	 * Even though the buffer is a full block, to avoid the potential corruption from a torn
	 * write, the entire encoding must fit in the first sector.
	 */
	return wrap_buffer(codec->encoded_super_block, VDO_SECTOR_SIZE, 0, &codec->block_buffer);
}

/**
 * vdo_destroy_super_block_codec() - Free resources in a super block codec.
 * @codec: The codec to clean up.
 */
void vdo_destroy_super_block_codec(struct super_block_codec *codec)
{
	free_buffer(UDS_FORGET(codec->block_buffer));
	free_buffer(UDS_FORGET(codec->component_buffer));
	UDS_FREE(codec->encoded_super_block);
}

/**
 * vdo_encode_super_block() - Encode a super block into its on-disk representation.
 * @codec: The super block codec.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_encode_super_block(struct super_block_codec *codec)
{
	size_t component_data_size;
	u32 checksum;
	struct header header = SUPER_BLOCK_HEADER_12_0;
	struct buffer *buffer = codec->block_buffer;
	int result;

	result = reset_buffer_end(buffer, 0);
	if (result != VDO_SUCCESS)
		return result;

	component_data_size = content_length(codec->component_buffer);

	/* Encode the header. */
	header.size += component_data_size;
	result = vdo_encode_header(&header, buffer);
	if (result != UDS_SUCCESS)
		return result;

	/* Copy the already-encoded component data. */
	result = put_bytes(buffer,
			   component_data_size,
			   get_buffer_contents(codec->component_buffer));
	if (result != UDS_SUCCESS)
		return result;

	/* Compute and encode the checksum. */
	checksum = vdo_crc32(codec->encoded_super_block, content_length(buffer));
	result = put_u32_le_into_buffer(buffer, checksum);
	if (result != UDS_SUCCESS)
		return result;

	return UDS_SUCCESS;
}

/**
 * vdo_decode_super_block() - Decode a super block from its on-disk representation.
 * @codec: The super block to decode.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_super_block(struct super_block_codec *codec)
{
	struct header header;
	int result;
	size_t component_data_size;
	u32 checksum, saved_checksum;

	/* Reset the block buffer to start decoding the entire first sector. */
	struct buffer *buffer = codec->block_buffer;

	clear_buffer(buffer);

	/* Decode and validate the header. */
	result = vdo_decode_header(buffer, &header);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_header(&SUPER_BLOCK_HEADER_12_0, &header, false, __func__);
	if (result != VDO_SUCCESS)
		return result;

	if (header.size > content_length(buffer))
		/*
		 * We can't check release version or checksum until we know the content size, so we
		 * have to assume a version mismatch on unexpected values.
		 */
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "super block contents too large: %zu",
					      header.size);

	/* Restrict the buffer to the actual payload bytes that remain. */
	result = reset_buffer_end(buffer, uncompacted_amount(buffer) + header.size);
	if (result != VDO_SUCCESS)
		return result;

	/* The component data is all the rest, except for the checksum. */
	component_data_size = content_length(buffer) - sizeof(u32);
	result = put_buffer(codec->component_buffer, buffer, component_data_size);
	if (result != VDO_SUCCESS)
		return result;

	/* Checksum everything up to but not including the saved checksum itself. */
	checksum = vdo_crc32(codec->encoded_super_block, uncompacted_amount(buffer));

	/* Decode and verify the saved checksum. */
	result = get_u32_le_from_buffer(buffer, &saved_checksum);
	if (result != VDO_SUCCESS)
		return result;

	result = ASSERT(content_length(buffer) == 0,
			"must have decoded entire superblock payload");
	if (result != VDO_SUCCESS)
		return result;

	return ((checksum != saved_checksum) ? VDO_CHECKSUM_MISMATCH : VDO_SUCCESS);
}
