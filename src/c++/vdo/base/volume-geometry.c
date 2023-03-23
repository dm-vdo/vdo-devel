// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "volume-geometry.h"

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

#include "constants.h"
#include "encodings.h"
#ifndef __KERNEL__
#include "physicalLayer.h"
#endif /* not __KERNEL__ */
#include "release-versions.h"
#include "status-codes.h"
#include "types.h"

enum {
	MAGIC_NUMBER_SIZE = 8,
	DEFAULT_GEOMETRY_BLOCK_VERSION = 5,
};

struct geometry_block {
	char magic_number[MAGIC_NUMBER_SIZE];
	struct packed_header header;
	u32 checksum;
} __packed;

static const struct header GEOMETRY_BLOCK_HEADER_5_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 5,
		.minor_version = 0,
	},
	/*
	 * Note: this size isn't just the payload size following the header, like it is everywhere
	 * else in VDO.
	 */
	.size = sizeof(struct geometry_block) + sizeof(struct volume_geometry),
};

static const struct header GEOMETRY_BLOCK_HEADER_4_0 = {
	.id = VDO_GEOMETRY_BLOCK,
	.version = {
		.major_version = 4,
		.minor_version = 0,
	},
	/*
	 * Note: this size isn't just the payload size following the header, like it is everywhere
	 * else in VDO.
	 */
	.size = sizeof(struct geometry_block) + sizeof(struct volume_geometry_4_0),
};

static const u8 MAGIC_NUMBER[MAGIC_NUMBER_SIZE + 1] = "dmvdo001";

static const release_version_number_t COMPATIBLE_RELEASE_VERSIONS[] = {
	VDO_MAGNESIUM_RELEASE_VERSION_NUMBER,
	VDO_ALUMINUM_RELEASE_VERSION_NUMBER,
};

/**
 * is_loadable_release_version() - Determine whether the supplied release version can be understood
 *                                 by the VDO code.
 * @version: The release version number to check.
 *
 * Return: True if the given version can be loaded.
 */
static inline bool is_loadable_release_version(release_version_number_t version)
{
	unsigned int i;

	if (version == VDO_CURRENT_RELEASE_VERSION_NUMBER)
		return true;

	for (i = 0; i < ARRAY_SIZE(COMPATIBLE_RELEASE_VERSIONS); i++)
		if (version == COMPATIBLE_RELEASE_VERSIONS[i])
			return true;

	return false;
}

/**
 * decode_volume_geometry() - Decode the on-disk representation of a volume geometry from a buffer.
 * @buffer: A buffer to decode from.
 * @offset: The offset in the buffer at which to decode.
 * @geometry: The structure to receive the decoded fields.
 * @version: The geometry block version to decode.
 */
static void
decode_volume_geometry(u8 *buffer, size_t *offset, struct volume_geometry *geometry, u32 version)
{
	release_version_number_t release_version;
	enum volume_region_id id;
	nonce_t nonce;
	block_count_t bio_offset = 0;
	u32 mem;
	bool sparse;

	decode_u32_le(buffer, offset, &release_version);
	decode_u64_le(buffer, offset, &nonce);
	geometry->release_version = release_version;
	geometry->nonce = nonce;

	memcpy((unsigned char *) &geometry->uuid, buffer + *offset, sizeof(uuid_t));
	*offset += sizeof(uuid_t);

	if (version > 4)
		decode_u64_le(buffer, offset, &bio_offset);
	geometry->bio_offset = bio_offset;

	for (id = 0; id < VDO_VOLUME_REGION_COUNT; id++) {
		physical_block_number_t start_block;
		enum volume_region_id saved_id;

		decode_u32_le(buffer, offset, &saved_id);
		decode_u64_le(buffer, offset, &start_block);

		geometry->regions[id] = (struct volume_region) {
			.id = saved_id,
			.start_block = start_block,
		};
	}

	decode_u32_le(buffer, offset, &mem);
	*offset += sizeof(u32);
	sparse = buffer[(*offset)++];

	geometry->index_config = (struct index_config) {
		.mem = mem,
		.sparse = sparse,
	};
}

#if (defined(VDO_USER) || defined(INTERNAL))
/**
 * encode_volume_geometry() - Encode the on-disk representation of a volume geometry into a buffer.
 * @buffer: A buffer to store the encoding.
 * @offset: The offset in the buffer at which to encode.
 * @geometry: The geometry to encode.
 * @version: The geometry block version to encode.
 */
static void encode_volume_geometry(u8 *buffer,
				   size_t *offset,
				   const struct volume_geometry *geometry,
				   u32 version)
{
	enum volume_region_id id;

	encode_u32_le(buffer, offset, geometry->release_version);
	encode_u64_le(buffer, offset, geometry->nonce);
	memcpy(buffer + *offset, (unsigned char *) &geometry->uuid, sizeof(uuid_t));
	*offset += sizeof(uuid_t);

	if (version > 4)
		encode_u64_le(buffer, offset, geometry->bio_offset);

	for (id = 0; id < VDO_VOLUME_REGION_COUNT; id++) {
		encode_u32_le(buffer, offset, geometry->regions[id].id);
		encode_u64_le(buffer, offset, geometry->regions[id].start_block);
	}

	encode_u32_le(buffer, offset, geometry->index_config.mem);
	encode_u32_le(buffer, offset, 0);

	if (geometry->index_config.sparse)
		buffer[(*offset)++] = 1;
	else
		buffer[(*offset)++] = 0;
}

#endif /* VDO_USER */
/**
 * vdo_parse_geometry_block() - Decode and validate an encoded geometry block.
 * @block: The encoded geometry block.
 * @geometry: The structure to receive the decoded fields.
 */
int __must_check vdo_parse_geometry_block(u8 *block, struct volume_geometry *geometry)
{
	u32 checksum, saved_checksum;
	struct header header;
	size_t offset = 0;
	int result;

	if (memcmp(block, MAGIC_NUMBER, MAGIC_NUMBER_SIZE) != 0)
		return VDO_BAD_MAGIC;
	offset += MAGIC_NUMBER_SIZE;

	vdo_decode_header(block, &offset, &header);
	if (header.version.major_version <= 4)
		result = vdo_validate_header(&GEOMETRY_BLOCK_HEADER_4_0, &header, true, __func__);
	else
		result = vdo_validate_header(&GEOMETRY_BLOCK_HEADER_5_0, &header, true, __func__);
	if (result != VDO_SUCCESS)
		return result;

	decode_volume_geometry(block, &offset, geometry, header.version.major_version);

	result = ASSERT(header.size == offset + sizeof(u32),
			"should have decoded up to the geometry checksum");
	if (result != VDO_SUCCESS)
		return result;

	/* Decode and verify the checksum. */
	checksum = vdo_crc32(block, offset);
	decode_u32_le(block, &offset, &saved_checksum);

	if (!is_loadable_release_version(geometry->release_version))
		return uds_log_error_strerror(VDO_UNSUPPORTED_VERSION,
					      "release version %d cannot be loaded",
					      geometry->release_version);

	return ((checksum == saved_checksum) ? VDO_SUCCESS : VDO_CHECKSUM_MISMATCH);
}

#ifndef __KERNEL__
/**
 * vdo_load_volume_geometry() - Load the volume geometry from a layer.
 * @layer: The layer to read and parse the geometry from.
 * @geometry: The structure to receive the decoded fields.
 */
int vdo_load_volume_geometry(PhysicalLayer *layer, struct volume_geometry *geometry)
{
	char *block;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block", &block);
	if (result != VDO_SUCCESS)
		return result;

	result = layer->reader(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, block);
	if (result != VDO_SUCCESS) {
		UDS_FREE(block);
		return result;
	}

	result = vdo_parse_geometry_block((u8 *) block, geometry);
	UDS_FREE(block);
	return result;
}

/**
 * vdo_compute_index_blocks() - Compute the index size in blocks from the index_config.
 * @index_config: The index config.
 * @index_blocks_ptr: A pointer to return the index size in blocks.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_compute_index_blocks(const struct index_config *index_config,
			     block_count_t *index_blocks_ptr)
{
	int result;
	u64 index_bytes;
	block_count_t index_blocks;
	struct uds_parameters uds_parameters = {
		.memory_size = index_config->mem,
		.sparse = index_config->sparse,
	};

	result = uds_compute_index_size(&uds_parameters, &index_bytes);
	if (result != UDS_SUCCESS)
		return uds_log_error_strerror(result, "error computing index size");

	index_blocks = index_bytes / VDO_BLOCK_SIZE;
	if ((((u64) index_blocks) * VDO_BLOCK_SIZE) != index_bytes)
		return uds_log_error_strerror(VDO_PARAMETER_MISMATCH,
					      "index size must be a multiple of block size %d",
					      VDO_BLOCK_SIZE);

	*index_blocks_ptr = index_blocks;
	return VDO_SUCCESS;
}

/**
 * vdo_initialize_volume_geometry() - Initialize a volume_geometry for a VDO.
 * @nonce: The nonce for the VDO.
 * @uuid: The uuid for the VDO.
 * @index_config: The index config of the VDO.
 * @geometry: The geometry being initialized.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_initialize_volume_geometry(nonce_t nonce,
				   uuid_t *uuid,
				   const struct index_config *index_config,
				   struct volume_geometry *geometry)
{
	int result;
	block_count_t index_size = 0;

	if (index_config != NULL) {
		result = vdo_compute_index_blocks(index_config, &index_size);
		if (result != VDO_SUCCESS)
			return result;
	}

	*geometry = (struct volume_geometry) {
		.release_version = VDO_CURRENT_RELEASE_VERSION_NUMBER,
		.nonce = nonce,
		.bio_offset = 0,
		.regions = {
			[VDO_INDEX_REGION] = {
				.id = VDO_INDEX_REGION,
				.start_block = 1,
			},
			[VDO_DATA_REGION] = {
				.id = VDO_DATA_REGION,
				.start_block = 1 + index_size,
			}
		}
	};

	uuid_copy(geometry->uuid, *uuid);
	if (index_size > 0)
		memcpy(&geometry->index_config, index_config, sizeof(struct index_config));

	return VDO_SUCCESS;
}

/**
 * vdo_clear_volume_geometry() - Zero out the geometry on a layer.
 * @layer: The layer to clear.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_clear_volume_geometry(PhysicalLayer *layer)
{
	char *block;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry block", &block);
	if (result != VDO_SUCCESS)
		return result;

	result = layer->writer(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, block);
	UDS_FREE(block);
	return result;
}

/**
 * vdo_write_volume_geometry() - Write a geometry block for a VDO.
 * @layer: The layer on which to write.
 * @geometry: The volume_geometry to be written.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_write_volume_geometry(PhysicalLayer *layer, struct volume_geometry *geometry)
{
	return vdo_write_volume_geometry_with_version(layer,
						      geometry,
						      DEFAULT_GEOMETRY_BLOCK_VERSION);
}

/**
 * vdo_write_volume_geometry_with_version() - Write a specific version of geometry block for a VDO.
 * @layer: The layer on which to write.
 * @geometry: The VolumeGeometry to be written.
 * @version: The version of VolumeGeometry to write.
 *
 * Return: VDO_SUCCESS or an error.
 */
int __must_check vdo_write_volume_geometry_with_version(PhysicalLayer *layer,
							struct volume_geometry *geometry,
							u32 version)
{
	u8 *block;
	const struct header *header;
	size_t offset = 0;
	u32 checksum;
	int result;

	result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "geometry", (char **) &block);
	if (result != VDO_SUCCESS)
		return result;

	memcpy(block, MAGIC_NUMBER, MAGIC_NUMBER_SIZE);
	offset += MAGIC_NUMBER_SIZE;

	header = ((version <= 4) ? &GEOMETRY_BLOCK_HEADER_4_0 : &GEOMETRY_BLOCK_HEADER_5_0);
	vdo_encode_header(block, &offset, header);

	encode_volume_geometry(block, &offset, geometry, version);

	result = ASSERT(header->size == offset + sizeof(u32),
			"should have decoded up to the geometry checksum");
	if (result != VDO_SUCCESS) {
		UDS_FREE(block);
		return result;
	}

	checksum = vdo_crc32(block, offset);
	encode_u32_le(block, &offset, checksum);

	result = layer->writer(layer, VDO_GEOMETRY_BLOCK_LOCATION, 1, (char *) block);
	UDS_FREE(block);
	return result;
}
#endif /* not __KERNEL__ */
