// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-component-states.h"

#include "buffer.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map-format.h"
#include "constants.h"
#include "header.h"
#include "slab-depot-format.h"
#include "status-codes.h"
#include "types.h"
#include "vdo-component.h"
#include "vdo-layout.h"

const struct header VDO_RECOVERY_JOURNAL_HEADER_7_0 = {
	.id = VDO_RECOVERY_JOURNAL,
	.version = {
			.major_version = 7,
			.minor_version = 0,
		},
	.size = sizeof(struct recovery_journal_state_7_0),
};

const struct version_number VDO_VOLUME_VERSION_67_0 = {
	.major_version = 67,
	.minor_version = 0,
};

/**
 * vdo_get_recovery_journal_encoded_size() - Get the size of the encoded state of a recovery
 *                                           journal.
 *
 * Return: the encoded size of the journal's state.
 */
size_t vdo_get_recovery_journal_encoded_size(void)
{
	return VDO_ENCODED_HEADER_SIZE + sizeof(struct recovery_journal_state_7_0);
}

/**
 * vdo_encode_recovery_journal_state_7_0() - Encode the state of a recovery journal.
 * @state: The recovery journal state.
 * @buffer: The buffer to encode into.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_encode_recovery_journal_state_7_0(struct recovery_journal_state_7_0 state,
					  struct buffer *buffer)
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
 * vdo_decode_recovery_journal_state_7_0() - Decode the state of a recovery journal saved in a
 *                                           buffer.
 * @buffer: The buffer containing the saved state.
 * @state: A pointer to a recovery journal state to hold the result of a successful decode.
 *
 * Return: VDO_SUCCESS or an error code.
 */
int vdo_decode_recovery_journal_state_7_0(struct buffer *buffer,
					  struct recovery_journal_state_7_0 *state)
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
	case VDO_JOURNAL_DATA_DECREMENT:
		return "data decrement";

	case VDO_JOURNAL_DATA_INCREMENT:
		return "data increment";

	case VDO_JOURNAL_BLOCK_MAP_DECREMENT:
		return "block map decrement";

	case VDO_JOURNAL_BLOCK_MAP_INCREMENT:
		return "block map increment";

	default:
		return "unknown journal operation";
	}
}

/**
 * vdo_destroy_component_states() - Clean up any allocations in a
 *                                  vdo_component_states.
 * @states: The component states to destroy.
 */
void vdo_destroy_component_states(struct vdo_component_states *states)
{
	if (states == NULL)
		return;

	vdo_free_fixed_layout(UDS_FORGET(states->layout));
}

/**
 * decode_components() - Decode the components now that we know the component
 *                       data is a version we understand.
 * @buffer: The buffer being decoded.
 * @states: An object to hold the successfully decoded state.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
decode_components(struct buffer *buffer, struct vdo_component_states *states)
{
	int result = vdo_decode_component(buffer, &states->vdo);

	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_fixed_layout(buffer, &states->layout);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_recovery_journal_state_7_0(buffer,
						       &states->recovery_journal);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_slab_depot_state_2_0(buffer, &states->slab_depot);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_decode_block_map_state_2_0(buffer, &states->block_map);
	if (result != VDO_SUCCESS)
		return result;

	ASSERT_LOG_ONLY((content_length(buffer) == 0),
			"All decoded component data was used");
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
	result = vdo_decode_version_number(buffer, &states->volume_version);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_validate_version(VDO_VOLUME_VERSION_67_0,
				      states->volume_version,
				      "volume");
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
 * vdo_validate_component_states() - Validate the decoded super block
 *                                   configuration.
 * @states: The state decoded from the super block.
 * @geometry_nonce: The nonce from the geometry block.
 * @physical_size: The minimum block count of the underlying storage.
 * @logical_size: The expected logical size of the VDO, or 0 if the
 *                logical size may be unspecified.
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

	return vdo_validate_config(&states->vdo.config,
				   physical_size,
				   logical_size);
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
		vdo_get_component_encoded_size() +
		vdo_get_fixed_layout_encoded_size(layout) +
		vdo_get_recovery_journal_encoded_size() +
		vdo_get_slab_depot_encoded_size() +
		vdo_get_block_map_encoded_size());
}

/**
 * vdo_encode_component_states() - Encode the state of all vdo components for
 *                                 writing in the super block.
 * @buffer: The buffer to encode into.
 * @states: The states to encode.
 */
int vdo_encode_component_states(struct buffer *buffer,
				const struct vdo_component_states *states)
{
	size_t expected_size;
	int result = reset_buffer_end(buffer, 0);

	if (result != UDS_SUCCESS)
		return result;

	result = put_u32_le_into_buffer(buffer, states->release_version);
	if (result != UDS_SUCCESS)
		return result;

	result = vdo_encode_version_number(states->volume_version, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_component(states->vdo, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_fixed_layout(states->layout, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_recovery_journal_state_7_0(states->recovery_journal, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_slab_depot_state_2_0(states->slab_depot, buffer);
	if (result != VDO_SUCCESS)
		return result;

	result = vdo_encode_block_map_state_2_0(states->block_map, buffer);
	if (result != VDO_SUCCESS)
		return result;

	expected_size = get_component_data_size(states->layout);
	ASSERT_LOG_ONLY((content_length(buffer) == expected_size),
			"All super block component data was encoded");
	return VDO_SUCCESS;
}
