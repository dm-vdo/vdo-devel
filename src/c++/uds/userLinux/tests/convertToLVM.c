/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/dm-bufio.h>
#include <linux/err.h>

#include "config.h"
#include "errors.h"
#include "geometry.h"
#include "index.h"
#include "indexer.h"
#include "index-layout.h"
#include "index-session.h"
#include "logger.h"
#include "memory-alloc.h"
#include "volume.h"

#include "convertToLVM.h"

/**
 * Move the data for physical chapter 0 to a new physical location.
 *
 * @param volume        The volume
 * @param layout        The index layout
 * @param new_physical  The new physical chapter number to move to
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check move_chapter(struct volume *volume,
				     struct index_layout *layout,
				     uint64_t new_physical)
{
	struct index_geometry *geometry = volume->geometry;
	struct dm_bufio_client *client;
	struct dm_buffer *buffer;
	u8 *data;
	int result = UDS_SUCCESS;
	unsigned int page;

	result = uds_open_volume_bufio(layout,
				       geometry->bytes_per_page,
				       0,
				       &client);
	if (result != UDS_SUCCESS) {
		return result;
	}
	for (page = 0; page < geometry->pages_per_chapter; page++) {
		unsigned int physical_page =
			map_to_physical_page(geometry, 0, page);

		data = dm_bufio_read(client, physical_page, &buffer);
		if (IS_ERR(data)) {
			return vdo_log_warning_strerror(-PTR_ERR(data),
							"error reading physical page %u",
							physical_page);
		}
		physical_page =
			map_to_physical_page(geometry, new_physical, page);
		dm_bufio_release_move(buffer, physical_page);

		dm_bufio_mark_buffer_dirty(buffer);
		dm_bufio_release(buffer);
	}
	result = -dm_bufio_write_dirty_buffers(client);
	dm_bufio_client_destroy(client);
	return UDS_SUCCESS;
}

/**
 * Destroy the index session after an error.
 *
 * @param session  The index session
 **/
static void cleanup_session(struct uds_index_session *session)
{
	if (session != NULL) {
		// This can produce an error when the index is not open.
		int result = uds_close_index(session);
		if (result != UDS_SUCCESS) {
			vdo_log_warning_strerror(result, "Error closing index");
		}
		result = uds_destroy_index_session(session);
		if (result != UDS_SUCCESS) {
			vdo_log_warning_strerror(result, "Error closing index");
		}
	}
}

/**
 * Copy the index page map entries corresponding to physical chapter 0 to a new
 * location if necessary, and then shift the array of entries down to eliminate
 * the old entries for physical chapter 0. When saving the page map, the end of
 * the entries array will be ignored.
 *
 * @param volume        The volume
 * @param new_physical  The new physical chapter slot to move to
 *
 * @return UDS_SUCCESS or an error code
 **/
static int reduce_index_page_map(struct volume *volume, uint64_t new_physical)
{
	struct index_page_map *map = volume->index_page_map;
	struct index_geometry *geometry = volume->geometry;
	int entries_per_chapter = map->entries_per_chapter;
	int reduced_entries =
		(geometry->chapters_per_volume - 1) * entries_per_chapter;

	// Copy slot entries for the moved chapter to the new location.
	if (new_physical > 0) {
		size_t slot = new_physical * entries_per_chapter;
		size_t chapter_slot_size =
			sizeof(uint16_t) * entries_per_chapter;

		memcpy(&map->entries[slot], map->entries, chapter_slot_size);
	}

	// Shift the entries down to match the new set of chapters.
	memmove(map->entries, &map->entries[entries_per_chapter],
		reduced_entries * sizeof(uint16_t));

	return UDS_SUCCESS;
}

/**********************************************************************/
int uds_convert_to_lvm(struct uds_parameters *parameters,
		       size_t freed_space,
		       off_t *chapter_size)
{
	struct uds_index_session *session = NULL;
	struct uds_configuration *index_config;
	struct uds_index *index;
	struct index_layout *layout;
	struct volume *volume;
	uint64_t oldest;
	uint64_t newest;
	unsigned int chapters_per_volume;
	uint64_t remapped_virtual;
	unsigned int new_physical;
	size_t bytes_per_chapter;

	int result = uds_create_index_session(&session);
	if (result != UDS_SUCCESS) {
		return result;
	}
        parameters->zone_count = 1;
	result = uds_open_index(UDS_NO_REBUILD, parameters, session);
	if (result != UDS_SUCCESS) {
		cleanup_session(session);
		return result;
	}

	index = session->index;
	layout = index->layout;
	volume = index->volume;
	oldest = index->oldest_virtual_chapter;
	newest = index->newest_virtual_chapter;
	chapters_per_volume = volume->geometry->chapters_per_volume;

	bytes_per_chapter = volume->geometry->bytes_per_page *
		volume->geometry->pages_per_chapter;
	result = VDO_ASSERT(freed_space <= bytes_per_chapter,
			    "cannot free more than %zu bytes (%zu requested)",
			    bytes_per_chapter, freed_space);
	if (result != VDO_SUCCESS) {
		cleanup_session(session);
		return result;
	}

	vdo_log_info("virtual chapters %llu to %llu are valid\n",
		     (long long) oldest,
		     (long long) newest);

	if (newest - oldest > chapters_per_volume - 2) {
		uds_forget_chapter(volume, oldest);
		index->oldest_virtual_chapter++;
	}

	// Remap the chapter currently in physical chapter 0.
	remapped_virtual = newest - (newest % chapters_per_volume);
	new_physical = (newest + 1) % chapters_per_volume;

	result = reduce_index_page_map(volume, new_physical);
	if (result != UDS_SUCCESS) {
		cleanup_session(session);
		return result;
	}

	if (new_physical == 0) {
		/*
		 * We've already expired the oldest chapter. But pretend we
		 * moved the next virtual chapter to where it should go.
		 * This simplifies the virtual to physical mapping math.
		 */
		remapped_virtual += chapters_per_volume;
		new_physical = 1;
	} else if (remapped_virtual != newest) {
		// The open chapter has no state in the volume to move.
		result = move_chapter(volume, layout, new_physical);
		if (result != UDS_SUCCESS) {
			cleanup_session(session);
			return result;
		}
	}

	if (parameters->memory_size == UDS_MEMORY_CONFIG_256MB) {
		parameters->memory_size = UDS_MEMORY_CONFIG_REDUCED_256MB;
	} else if (parameters->memory_size == UDS_MEMORY_CONFIG_512MB) {
		parameters->memory_size = UDS_MEMORY_CONFIG_REDUCED_512MB;
	} else if (parameters->memory_size == UDS_MEMORY_CONFIG_768MB) {
		parameters->memory_size = UDS_MEMORY_CONFIG_REDUCED_768MB;
	} else {
		parameters->memory_size |= UDS_MEMORY_CONFIG_REDUCED;
	}

	result = uds_make_configuration(parameters, &index_config);
	if (result != UDS_SUCCESS) {
		cleanup_session(session);
		return result;
	}

	index_config->geometry->remapped_virtual = remapped_virtual;
	index_config->geometry->remapped_physical = new_physical - 1;

	*volume->geometry = *index_config->geometry;
	vdo_log_debug("Saving updated layout and writing index configuration");
	result = update_uds_layout(layout, index_config, freed_space,
				   bytes_per_chapter);
	uds_free_configuration(index_config);
	if (result != UDS_SUCCESS) {
		cleanup_session(session);
		return result;
	}

	*chapter_size = bytes_per_chapter;

	// Force a save, even though no new requests have been processed, so
	// that the save areas get updated
	index->need_to_save = true;
	cleanup_session(session);
	return UDS_SUCCESS;
}
