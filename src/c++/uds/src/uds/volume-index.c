// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "volume-index.h"

#include <linux/log2.h>

#include "buffer.h"
#include "compiler.h"
#include "config.h"
#include "errors.h"
#include "geometry.h"
#include "hash-utils.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"
#include "uds-threads.h"

/*
 * volume-index005:
 *
 * The volume index is a kept as a delta index where the payload is a
 * chapter number.  The volume index adds 2 basic functions to the delta
 * index:
 *
 *  (1) How to get the delta list number and address out of the record name.
 *
 *  (2) Dealing with chapter numbers, and especially the lazy flushing of
 *      chapters from the index.
 *
 * There are three ways of expressing chapter numbers: virtual, index, and
 * rolling.  The interface to the the volume index uses virtual chapter
 * numbers, which are 64 bits long.  We do not store such large values in
 * memory, so we internally use a binary value using the minimal number of
 * bits.
 *
 * The delta index stores the index chapter number, which is the low-order
 * bits of the virtual chapter number.
 *
 * When we need to deal with ordering of index chapter numbers, we roll the
 * index chapter number around so that the smallest one we are using has
 * the representation 0.  See convert_index_to_virtual() or
 * flush_invalid_entries() for an example of this technique.
 */
/*
 * volume-index006:
 *
 * The volume index is a kept as a wrapper around 2 volume index
 * implementations, one for dense chapters and one for sparse chapters.
 * Methods will be routed to one or the other, or both, depending on the
 * method and data passed in.
 *
 * The volume index is divided into zones, and in normal operation there is
 * one thread operating on each zone.  Any operation that operates on all
 * the zones needs to do its operation at a safe point that ensures that
 * only one thread is operating on the volume index.
 *
 * The only multithreaded operation supported by the sparse volume index is
 * the lookup_volume_index_name() method.  It is called by the thread that
 * assigns an index request to the proper zone, and needs to do a volume
 * index query for sampled record names.  The zone mutexes are used to make
 * this lookup operation safe.
 */

struct sub_index_parameters {
	unsigned int address_bits;     /* Number of bits in address mask */
	unsigned int chapter_bits;     /* Number of bits in chapter number */
	unsigned int mean_delta;       /* The mean delta */
	unsigned long num_delta_lists; /* The number of delta lists */
	unsigned long num_chapters;    /* Number of chapters used */
	size_t num_bits_per_chapter;   /* Number of bits per chapter */
	size_t memory_size;            /* Number of bytes of delta list memory */
	size_t target_free_size;       /* Number of free bytes we desire */
};

struct split_config {
	struct configuration hook_config; /* Describe hook part of the index */
	struct geometry hook_geometry;
	struct configuration non_hook_config; /* Describe non-hook part of the */
					      /* index */
	struct geometry non_hook_geometry;
};

struct volume_sub_index_zone {
	uint64_t virtual_chapter_low;   /* The lowest virtual chapter indexed */
	uint64_t virtual_chapter_high;  /* The highest virtual chapter indexed */
	long num_early_flushes;         /* The number of early flushes */
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct volume_sub_index {
	struct delta_index delta_index;   /* The delta index */
	uint64_t *flush_chapters;         /* The first chapter to be flushed */
	struct volume_sub_index_zone *zones; /* The Zones */
	uint64_t volume_nonce;            /* The volume nonce */
	uint64_t chapter_zone_bits;       /* Expected size of a chapter */
				          /* (per zone) */
	uint64_t max_zone_bits;           /* Maximum size index (per zone) */
	unsigned int address_bits;        /* Number of bits in address mask */
	unsigned int address_mask;        /* Mask to get address within delta */
				          /* list */
	unsigned int chapter_bits;        /* Number of bits in chapter number */
	unsigned int chapter_mask;        /* Largest storable chapter number */
	unsigned int num_chapters;        /* Number of chapters used */
	unsigned int num_delta_lists;     /* The number of delta lists */
	unsigned int num_zones; /* The number of zones */
};

struct volume_index_zone {
	struct mutex hook_mutex; /* Protects the sampled index in this zone */
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct volume_index {
	unsigned int sparse_sample_rate;     /* The sparse sample rate */
	unsigned int num_zones;              /* The number of zones */
	struct volume_sub_index vi_non_hook; /* The non-hook index */
	struct volume_sub_index vi_hook;     /* Hook index == sample index */
	struct volume_index_zone *zones;     /* The zones */
};

struct chapter_range {
	unsigned int chapter_start;  /* The first chapter */
	unsigned int chapter_count;  /* The number of chapters */
};

/**
 * Constants and structures for the saved volume index region. "MI5"
 * indicates volume index 005, and "-XXXX" is a number to increment
 * when the format of the data changes. The abbreviation MI5 is
 * derived from the name of a previous data structure that represented
 * the volume index region.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_START_5[] = "MI5-0005";

struct sub_index_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START_5 */
	uint64_t volume_nonce;
	uint64_t virtual_chapter_low;
	uint64_t virtual_chapter_high;
	unsigned int first_list;
	unsigned int num_lists;
};

/**
 * Constants and structures for the saved volume index region. "MI6"
 * indicates volume index 006, and "-XXXX" is a number to increment
 * when the format of the data changes. The abbreviation MI6 is
 * derived from the name of a previous data structure that represented
 * the volume index region.
 **/
static const char MAGIC_START_6[] = "MI6-0001";

struct volume_index_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START_6 */
	unsigned int sparse_sample_rate;
};

/* Constants for the magic byte of a volume_index_record */
static const byte volume_index_record_magic = 0xAA;
static const byte bad_magic = 0;

#ifdef TEST_INTERNAL
/*
 * In production, the default value for min_volume_index_delta_lists will be
 * replaced by MAX_ZONES*MAX_ZONES.  Some unit tests will replace
 * min_volume_index_delta_lists with the non-default value 1, because those
 * tests really want to run with a single delta list.
 */
unsigned int min_volume_index_delta_lists;

#endif /* TEST_INTERNAL */
/**
 * Extract the address from a block name.
 *
 * @param sub_index   The volume subindex
 * @param name        The block name
 *
 * @return the address
 **/
static INLINE unsigned int extract_address(const struct volume_sub_index *sub_index,
					   const struct uds_record_name *name)
{
	return extract_volume_index_bytes(name) & sub_index->address_mask;
}

/**
 * Extract the delta list number from a block name.
 *
 * @param sub_index   The volume subindex
 * @param name        The block name
 *
 * @return the delta list number
 **/
static INLINE unsigned int
extract_dlist_num(const struct volume_sub_index *sub_index,
		  const struct uds_record_name *name)
{
	uint64_t bits = extract_volume_index_bytes(name);

	return (bits >> sub_index->address_bits) % sub_index->num_delta_lists;
}

/**
 * Get the volume index zone containing a given volume index record
 *
 * @param record  The volume index record
 *
 * @return the volume index zone
 **/
static INLINE const struct volume_sub_index_zone *
get_zone_for_record(const struct volume_index_record *record)
{
	return &record->sub_index->zones[record->zone_number];
}

/**
 * Convert an index chapter number to a virtual chapter number.
 *
 * @param record         The volume index record
 * @param index_chapter  The index chapter number
 *
 * @return the virtual chapter number
 **/
static INLINE uint64_t
convert_index_to_virtual(const struct volume_index_record *record,
			 unsigned int index_chapter)
{
	const struct volume_sub_index_zone *volume_index_zone =
		get_zone_for_record(record);
	unsigned int rolling_chapter =
		((index_chapter - volume_index_zone->virtual_chapter_low) &
		 record->sub_index->chapter_mask);
	return volume_index_zone->virtual_chapter_low + rolling_chapter;
}

/**
 * Convert a virtual chapter number to an index chapter number.
 *
 * @param sub_index        The volume subindex
 * @param virtual_chapter  The virtual chapter number
 *
 * @return the index chapter number
 **/
static INLINE unsigned int
convert_virtual_to_index(const struct volume_sub_index *sub_index,
			 uint64_t virtual_chapter)
{
	return virtual_chapter & sub_index->chapter_mask;
}

/**
 * Determine whether a virtual chapter number is in the range being indexed
 *
 * @param record           The volume index record
 * @param virtual_chapter  The virtual chapter number
 *
 * @return true if the virtual chapter number is being indexed
 **/
static INLINE bool
is_virtual_chapter_indexed(const struct volume_index_record *record,
			   uint64_t virtual_chapter)
{
	const struct volume_sub_index_zone *volume_index_zone =
		get_zone_for_record(record);
	return ((virtual_chapter >= volume_index_zone->virtual_chapter_low) &&
		(virtual_chapter <= volume_index_zone->virtual_chapter_high));
}

static INLINE bool has_sparse(const struct volume_index *volume_index)
{
	return volume_index->sparse_sample_rate > 0;
}

/**
 * Determine whether a given record name is a hook.
 *
 * @param volume_index   The volume index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
bool is_volume_index_sample(const struct volume_index *volume_index,
			    const struct uds_record_name *name)
{
	if (!has_sparse(volume_index))  {
		return false;
	}

	return (extract_sampling_bytes(name) % volume_index->sparse_sample_rate) == 0;
}

/**
 * Get the subindex for the given record name
 *
 * @param volume_index   The volume index
 * @param name           The block name
 *
 * @return the subindex
 **/
static INLINE const struct volume_sub_index *
get_sub_index(const struct volume_index *volume_index,
	      const struct uds_record_name *name)
{
	return (is_volume_index_sample(volume_index, name) ?
			&volume_index->vi_hook :
			&volume_index->vi_non_hook);
}

/**
 * Find the volume index zone associated with a record name
 *
 * @param volume_index The volume index
 * @param name         The record name
 *
 * @return the zone that the record name belongs to
 **/
static unsigned int
get_volume_sub_index_zone(const struct volume_sub_index *sub_index,
			  const struct uds_record_name *name)
{
	unsigned int delta_list_number = extract_dlist_num(sub_index, name);

	return get_delta_zone_number(&sub_index->delta_index, delta_list_number);
}

unsigned int get_volume_index_zone(const struct volume_index *volume_index,
				   const struct uds_record_name *name)
{
	return get_volume_sub_index_zone(get_sub_index(volume_index, name), name);
}

static INLINE bool uses_sparse(const struct configuration *config)
{
	return is_sparse_geometry(config->geometry);
}

static int
compute_volume_index_parameters(const struct configuration *config,
				struct sub_index_parameters *params)
{
	enum { DELTA_LIST_SIZE = 256 };
	unsigned long invalid_chapters, address_span;
	unsigned long chapters_in_volume_index, entries_in_volume_index;
	unsigned long rounded_chapters;
	unsigned long delta_list_records;
	unsigned int num_addresses;
	uint64_t num_bits_per_index;
	size_t expected_index_size;
	/*
	 * For a given zone count, setting the the minimum number of delta
	 * lists to the square of the number of zones ensures that the
	 * distribution of delta lists over zones doesn't underflow, leaving
	 * the last zone with an invalid number of delta lists. See the
	 * explanation in initialize_delta_index(). Because we can restart with
	 * a different number of zones but the number of delta lists is
	 * invariant across restart, we must use the largest number of zones to
	 * compute this minimum.
	 */
	unsigned long min_delta_lists = MAX_ZONES * MAX_ZONES;
	struct geometry *geometry = config->geometry;
	unsigned long records_per_chapter = geometry->records_per_chapter;

#ifdef TEST_INTERNAL
	if (min_volume_index_delta_lists > 0) {
		min_delta_lists = min_volume_index_delta_lists;
	}
#endif /* TEST_INTERNAL */
	params->num_chapters = geometry->chapters_per_volume;
	/*
	 * Make sure that the number of delta list records in the
	 * volume index does not change when the volume is reduced by
	 * one chapter. This preserves the mapping from hash to volume
	 * index delta list.
	 */
	rounded_chapters = params->num_chapters;
	if (is_reduced_geometry(geometry))
		rounded_chapters += 1;
	delta_list_records = records_per_chapter * rounded_chapters;
	num_addresses = config->volume_index_mean_delta * DELTA_LIST_SIZE;
	params->num_delta_lists =
		max(delta_list_records / DELTA_LIST_SIZE, min_delta_lists);
	params->address_bits = bits_per(num_addresses - 1);
	params->chapter_bits = bits_per(rounded_chapters - 1);
	if ((unsigned int) params->num_delta_lists !=
	    params->num_delta_lists) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu delta lists",
						params->num_delta_lists);
	}
	if (params->address_bits > 31) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %u address bits",
						params->address_bits);
	}
	if (is_sparse_geometry(geometry)) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize dense volume index with %u sparse chapters",
						geometry->sparse_chapters_per_volume);
	}
	if (records_per_chapter == 0) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu records per chapter",
						records_per_chapter);
	}
	if (params->num_chapters == 0) {
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu chapters per volume",
						params->num_chapters);
	}

	/*
	 * We can now compute the probability that a delta list is not touched
	 * during the writing of an entire chapter.  The computation is:
	 *
	 * double p_not_touched = pow((double) (params->num_delta_lists - 1)
	 *                          / params->num_delta_lists,
	 *                          records_per_chapter);
	 *
	 * For the standard index sizes, about 78% of the delta lists are not
	 * touched, and therefore contain dead index entries that have not been
	 * eliminated by the lazy LRU processing.  We can then compute how many
	 * dead index entries accumulate over time.  The computation is:
	 *
	 * double invalid_chapters = p_not_touched / (1.0 - p_not_touched);
	 *
	 * For the standard index sizes, we will need about 3.5 chapters of
	 * space for the dead index entries in a 1K chapter index.  Since we do
	 * not want to do that floating point computation, we use 4 chapters
	 * per 1K of chapters.
	 */
	invalid_chapters = max(rounded_chapters / 256, 2UL);
	chapters_in_volume_index = rounded_chapters + invalid_chapters;
	entries_in_volume_index =
		records_per_chapter * chapters_in_volume_index;
	/* Compute the mean delta */
	address_span =
		(uint64_t) params->num_delta_lists << params->address_bits;
	params->mean_delta = address_span / entries_in_volume_index;
	/* Project how large we expect a chapter to be */
	params->num_bits_per_chapter =
		compute_delta_index_size(records_per_chapter,
					 params->mean_delta,
					 params->chapter_bits);
	/* Project how large we expect the index to be */
	num_bits_per_index =
		params->num_bits_per_chapter * chapters_in_volume_index;
	expected_index_size = num_bits_per_index / CHAR_BIT;
	/*
	 * Set the total memory to be 6% larger than the expected index size.
	 * We want this number to be large enough that the we do not do a great
	 * many rebalances as the list when the list is full.  We use
	 * VolumeIndex_p1 to tune this setting.
	 */
	params->memory_size = expected_index_size * 106 / 100;
	/* Set the target free size to 5% of the expected index size */
	params->target_free_size = expected_index_size / 20;
	return UDS_SUCCESS;
}

/**
 * Terminate and clean up the volume sub index
 *
 * @param sub_index The volume subindex to terminate
 **/
static void uninitialize_volume_sub_index(struct volume_sub_index *sub_index)
{
	UDS_FREE(sub_index->flush_chapters);
	sub_index->flush_chapters = NULL;
	UDS_FREE(sub_index->zones);
	sub_index->zones = NULL;
	uninitialize_delta_index(&sub_index->delta_index);
}

/**
 * Terminate and clean up the volume index
 *
 * @param volume_index The volume index to terminate
 **/
void free_volume_index(struct volume_index *volume_index)
{
	if (volume_index == NULL) {
		return;
	}

	if (volume_index->zones != NULL) {
		unsigned int zone;

		for (zone = 0; zone < volume_index->num_zones; zone++) {
			uds_destroy_mutex(&volume_index->zones[zone].hook_mutex);
		}
		UDS_FREE(volume_index->zones);
		volume_index->zones = NULL;
	}
	uninitialize_volume_sub_index(&volume_index->vi_non_hook);
	uninitialize_volume_sub_index(&volume_index->vi_hook);
	UDS_FREE(volume_index);
}

/**
 * Compute the number of bytes required to save a volume index of a given
 * configuration.
 *
 * @param config     The configuration of the volume index
 * @param num_bytes  The number of bytes required to save the volume index
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int
compute_volume_sub_index_save_bytes(const struct configuration *config,
				   size_t *num_bytes)
{
	struct sub_index_parameters params = { .address_bits = 0 };
	int result = compute_volume_index_parameters(config, &params);

	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * Saving a volume sub index needs a header plus one uint64_t per delta
	 * list plus the delta index.
	 */
	*num_bytes = (sizeof(struct sub_index_data) +
		      params.num_delta_lists * sizeof(uint64_t) +
		      compute_delta_index_save_bytes(params.num_delta_lists,
						     params.memory_size));
	return UDS_SUCCESS;
}

static int split_configuration(const struct configuration *config,
			       struct split_config *split)
{
	uint64_t sample_rate, num_chapters, num_sparse_chapters;
	uint64_t num_dense_chapters, sample_records;
	int result = ASSERT(config->geometry->sparse_chapters_per_volume != 0,
			    "cannot initialize sparse+dense volume index with no sparse chapters");
	if (result != UDS_SUCCESS) {
		return UDS_INVALID_ARGUMENT;
	}
	result = ASSERT(config->sparse_sample_rate != 0,
			"cannot initialize sparse+dense volume index with a sparse sample rate of %u",
			config->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return UDS_INVALID_ARGUMENT;
	}

	/* Start with copies of the base configuration */
	split->hook_config = *config;
	split->hook_geometry = *config->geometry;
	split->hook_config.geometry = &split->hook_geometry;
	split->non_hook_config = *config;
	split->non_hook_geometry = *config->geometry;
	split->non_hook_config.geometry = &split->non_hook_geometry;

	sample_rate = config->sparse_sample_rate;
	num_chapters = config->geometry->chapters_per_volume;
	num_sparse_chapters = config->geometry->sparse_chapters_per_volume;
	num_dense_chapters = num_chapters - num_sparse_chapters;
	sample_records = config->geometry->records_per_chapter / sample_rate;

	/* Adjust the number of records indexed for each chapter */
	split->hook_geometry.records_per_chapter = sample_records;
	split->non_hook_geometry.records_per_chapter -= sample_records;

	/* Adjust the number of chapters indexed */
	split->hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.chapters_per_volume = num_dense_chapters;
	return UDS_SUCCESS;
}

/**
 * Compute the number of bytes required to save a volume index of a given
 * configuration.
 *
 * @param config     The configuration of the volume index
 * @param num_bytes  The number of bytes required to save the volume index
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int compute_volume_index_save_bytes(const struct configuration *config,
					   size_t *num_bytes)
{
	size_t hook_bytes, non_hook_bytes;
	struct split_config split;
	int result = split_configuration(config, &split);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = compute_volume_sub_index_save_bytes(&split.hook_config,
						     &hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = compute_volume_sub_index_save_bytes(&split.non_hook_config,
						     &non_hook_bytes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	/*
	 * Saving a sparse volume index needs a header plus the hook index plus
	 * the non-hook index
	 */
	*num_bytes = sizeof(struct volume_index_data) + hook_bytes + non_hook_bytes;
	return UDS_SUCCESS;
}

int compute_volume_index_save_blocks(const struct configuration *config,
				     size_t block_size,
				     uint64_t *block_count)
{
	size_t num_bytes;
	int result = (uses_sparse(config) ?
			      compute_volume_index_save_bytes(config,
							      &num_bytes) :
			      compute_volume_sub_index_save_bytes(config,
								  &num_bytes));
	if (result != UDS_SUCCESS) {
		return result;
	}
	num_bytes += sizeof(struct delta_list_save_info);
	*block_count = DIV_ROUND_UP(num_bytes, block_size) + MAX_ZONES;
	return UDS_SUCCESS;
}

#ifdef TEST_INTERNAL
/**
 * Get the number of bytes used for volume index entries.
 *
 * @param volume_index The volume index
 *
 * @return The number of bytes in use
 **/
static size_t
get_volume_sub_index_memory_used(const struct volume_sub_index *sub_index)
{
	uint64_t bits = get_delta_index_bits_used(&sub_index->delta_index);

	return DIV_ROUND_UP(bits, CHAR_BIT);
}

/**
 * Get the number of bytes used for volume index entries.
 *
 * @param volume_index The volume index
 *
 * @return The number of bytes in use
 **/
size_t get_volume_index_memory_used(const struct volume_index *volume_index)
{
	size_t memory;

	memory = get_volume_sub_index_memory_used(&volume_index->vi_non_hook);
	if (has_sparse(volume_index)) {
		memory += get_volume_sub_index_memory_used(&volume_index->vi_hook);
	}

	return memory;
}

#endif /* TEST_INTERNAL */
/**
 * Flush an invalid entry from the volume index, advancing to the next
 * valid entry.
 *
 * @param record                      Updated to describe the next valid record
 * @param flush_range                 Range of chapters to flush from the index
 * @param next_chapter_to_invalidate  Updated to record the next chapter that
 *                                    we will need to invalidate
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int
flush_invalid_entries(struct volume_index_record *record,
		      struct chapter_range *flush_range,
		      unsigned int *next_chapter_to_invalidate)
{
	int result = next_delta_index_entry(&record->delta_entry);

	if (result != UDS_SUCCESS) {
		return result;
	}
	while (!record->delta_entry.at_end) {
		unsigned int index_chapter =
			get_delta_entry_value(&record->delta_entry);
		unsigned int relative_chapter =
			((index_chapter - flush_range->chapter_start) &
			 record->sub_index->chapter_mask);
		if (likely(relative_chapter >= flush_range->chapter_count)) {
			if (relative_chapter < *next_chapter_to_invalidate) {
				*next_chapter_to_invalidate = relative_chapter;
			}
			break;
		}
		result = remove_delta_index_entry(&record->delta_entry);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return UDS_SUCCESS;
}

/**
 * Find the delta index entry, or the insertion point for a delta index
 * entry, while processing chapter LRU flushing.
 *
 * @param record        Updated to describe the entry being looked for
 * @param list_number   The delta list number
 * @param key           The address field being looked for
 * @param flush_range   The range of chapters to flush from the index
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_volume_index_entry(struct volume_index_record *record,
				  unsigned int list_number,
				  unsigned int key,
				  struct chapter_range *flush_range)
{
	struct volume_index_record other_record;
	const struct volume_sub_index *sub_index = record->sub_index;
	unsigned int next_chapter_to_invalidate = sub_index->chapter_mask;

	int result = start_delta_index_search(&sub_index->delta_index, list_number,
					      0, &record->delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}
	do {
		result = flush_invalid_entries(record, flush_range,
					       &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} while (!record->delta_entry.at_end &&
			(key > record->delta_entry.key));

	result = remember_delta_index_offset(&record->delta_entry);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/* We probably found the record we want, but we need to keep going */
	other_record = *record;
	if (!other_record.delta_entry.at_end &&
	    (key == other_record.delta_entry.key)) {
		for (;;) {
			byte collision_name[UDS_RECORD_NAME_SIZE];

			result = flush_invalid_entries(&other_record,
						       flush_range,
						       &next_chapter_to_invalidate);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (other_record.delta_entry.at_end ||
			    !other_record.delta_entry.is_collision) {
				break;
			}
			result = get_delta_entry_collision(&other_record.delta_entry,
							   collision_name);
			if (result != UDS_SUCCESS) {
				return result;
			}
			if (memcmp(collision_name,
				   record->name,
				   UDS_RECORD_NAME_SIZE) == 0) {
				/*
				 * This collision record is the one we are
				 * looking for
				 */
				*record = other_record;
				break;
			}
		}
	}
	while (!other_record.delta_entry.at_end) {
		result = flush_invalid_entries(&other_record,
					       flush_range,
					       &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	next_chapter_to_invalidate += flush_range->chapter_start;
	next_chapter_to_invalidate &= sub_index->chapter_mask;
	flush_range->chapter_start = next_chapter_to_invalidate;
	flush_range->chapter_count = 0;
	return UDS_SUCCESS;
}

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this
 * entry.
 *
 * @param sub_index     The volume subindex to search
 * @param name          The record name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int get_volume_sub_index_record(struct volume_sub_index *sub_index,
				       const struct uds_record_name *name,
				       struct volume_index_record *record)
{
	int result;
	const struct volume_sub_index_zone *volume_index_zone;
	unsigned int address = extract_address(sub_index, name);
	unsigned int delta_list_number = extract_dlist_num(sub_index, name);
	uint64_t flush_chapter = sub_index->flush_chapters[delta_list_number];

	record->magic = volume_index_record_magic;
	record->sub_index = sub_index;
	record->mutex = NULL;
	record->name = name;
	record->zone_number =
		get_delta_zone_number(&sub_index->delta_index, delta_list_number);
	volume_index_zone = get_zone_for_record(record);

	if (flush_chapter < volume_index_zone->virtual_chapter_low) {
		struct chapter_range range;
		uint64_t flush_count =
			volume_index_zone->virtual_chapter_low - flush_chapter;
		range.chapter_start =
			convert_virtual_to_index(sub_index, flush_chapter);
		range.chapter_count = (flush_count > sub_index->chapter_mask ?
					       sub_index->chapter_mask + 1 :
					       flush_count);
		result = get_volume_index_entry(record, delta_list_number,
						address, &range);
		flush_chapter =
			convert_index_to_virtual(record, range.chapter_start);
		if (flush_chapter > volume_index_zone->virtual_chapter_high) {
			flush_chapter = volume_index_zone->virtual_chapter_high;
		}
		sub_index->flush_chapters[delta_list_number] = flush_chapter;
	} else {
		result = get_delta_index_entry(&sub_index->delta_index,
					       delta_list_number,
					       address,
					       name->name,
					       &record->delta_entry);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	record->is_found = (!record->delta_entry.at_end &&
			    (record->delta_entry.key == address));
	if (record->is_found) {
		unsigned int index_chapter =
			get_delta_entry_value(&record->delta_entry);
		record->virtual_chapter =
			convert_index_to_virtual(record, index_chapter);
	}
	record->is_collision = record->delta_entry.is_collision;
	return UDS_SUCCESS;
}

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block
 * name.  Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at
 * the proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter"
 * and "is_collision" fields reflect the entry found.
 * Calls to remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this
 * entry.
 *
 * @param volume_index  The volume index to search
 * @param name          The record name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
int get_volume_index_record(struct volume_index *volume_index,
			    const struct uds_record_name *name,
			    struct volume_index_record *record)
{
	int result;

	if (is_volume_index_sample(volume_index, name)) {
		/*
		 * We need to prevent a lookup_volume_index_name() happening
		 * while we are finding the volume index record.  Remember that
		 * because of lazy LRU flushing of the volume index,
		 * get_volume_index_record() is not a read-only operation.
		 */
		unsigned int zone = get_volume_sub_index_zone(&volume_index->vi_hook, name);
		struct mutex *mutex = &volume_index->zones[zone].hook_mutex;

		uds_lock_mutex(mutex);
		result = get_volume_sub_index_record(&volume_index->vi_hook, name, record);
		uds_unlock_mutex(mutex);
		/*
		 * Remember the mutex so that other operations on the
		 * volume_index_record can use it
		 */
		record->mutex = mutex;
	} else {
		result = get_volume_sub_index_record(&volume_index->vi_non_hook,
						     name,
						     record);
	}
	return result;
}

/**
 * Create a new record associated with a block name.
 *
 * @param record           The volume index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int put_volume_index_record(struct volume_index_record *record,
			    uint64_t virtual_chapter)
{
	int result;
	unsigned int address;
	const struct volume_sub_index *sub_index = record->sub_index;

	if (record->magic != volume_index_record_magic) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct volume_sub_index_zone *volume_index_zone =
			get_zone_for_record(record);
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot put record into chapter number %llu that is out of the valid range %llu to %llu",
						(unsigned long long) virtual_chapter,
						(unsigned long long) volume_index_zone->virtual_chapter_low,
						(unsigned long long) volume_index_zone->virtual_chapter_high);
	}
	address = extract_address(sub_index, record->name);
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = put_delta_index_entry(&record->delta_entry,
				       address,
				       convert_virtual_to_index(sub_index, virtual_chapter),
				       record->is_found ? record->name->name : NULL);
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	switch (result) {
	case UDS_SUCCESS:
		record->virtual_chapter = virtual_chapter;
		record->is_collision = record->delta_entry.is_collision;
		record->is_found = true;
		break;
	case UDS_OVERFLOW:
		uds_log_ratelimit(uds_log_warning_strerror,
				  UDS_OVERFLOW,
				  "Volume index entry dropped due to overflow condition");
		log_delta_index_entry(&record->delta_entry);
		break;
	default:
		break;
	}
	return result;
}

static INLINE int validate_record(struct volume_index_record *record)
{
	if (record->magic != volume_index_record_magic) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
	}
	if (!record->is_found) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"illegal operation on new record");
	}
	return UDS_SUCCESS;
}

/**
 * Remove an existing record.
 *
 * @param record      The volume index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int remove_volume_index_record(struct volume_index_record *record)
{
	int result = validate_record(record);

	if (result != UDS_SUCCESS) {
		return result;
	}
	/* Mark the record so that it cannot be used again */
	record->magic = bad_magic;
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = remove_delta_index_entry(&record->delta_entry);
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	return result;
}

/**
 * Set the open chapter number on a zone.  The volume index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param volume_index     The volume index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
static void
set_volume_sub_index_zone_open_chapter(struct volume_sub_index *sub_index,
				       unsigned int zone_number,
				       uint64_t virtual_chapter)
{
	uint64_t used_bits;
	struct volume_sub_index_zone *zone = &sub_index->zones[zone_number];

	/* Take care here to avoid underflow of an unsigned value. */
	zone->virtual_chapter_low =
		(virtual_chapter >= sub_index->num_chapters ?
		 virtual_chapter - sub_index->num_chapters + 1 :
		 0);
	zone->virtual_chapter_high = virtual_chapter;

	/* Check to see if the zone data has grown to be too large */
	used_bits = get_delta_zone_bits_used(&sub_index->delta_index,
					     zone_number);
	if (used_bits > sub_index->max_zone_bits) {
		/* Expire enough chapters to free the desired space */
		uint64_t expire_count =
			1 + (used_bits - sub_index->max_zone_bits) /
				    sub_index->chapter_zone_bits;
		if (expire_count == 1) {
			uds_log_ratelimit(uds_log_info,
					  "zone %u:  At chapter %llu, expiring chapter %llu early",
					  zone_number,
					  (unsigned long long) virtual_chapter,
					  (unsigned long long) zone->virtual_chapter_low);
			zone->num_early_flushes++;
			zone->virtual_chapter_low++;
		} else {
			uint64_t first_expired =
				zone->virtual_chapter_low;
			if (first_expired + expire_count <
			    zone->virtual_chapter_high) {
				zone->num_early_flushes +=
					expire_count;
				zone->virtual_chapter_low +=
					expire_count;
			} else {
				zone->num_early_flushes +=
					zone->virtual_chapter_high -
					zone->virtual_chapter_low;
				zone->virtual_chapter_low =
					zone->virtual_chapter_high;
			}
			uds_log_ratelimit(uds_log_info,
					  "zone %u:  At chapter %llu, expiring chapters %llu to %llu early",
					  zone_number,
					  (unsigned long long) virtual_chapter,
					  (unsigned long long) first_expired,
					  (unsigned long long) zone->virtual_chapter_low - 1);
		}
	}
}

void set_volume_index_zone_open_chapter(struct volume_index *volume_index,
					unsigned int zone_number,
					uint64_t virtual_chapter)
{
	struct mutex *mutex = &volume_index->zones[zone_number].hook_mutex;

	set_volume_sub_index_zone_open_chapter(&volume_index->vi_non_hook,
					       zone_number,
					       virtual_chapter);

	/*
	 * We need to prevent a lookup_volume_index_name() happening while we
	 * are changing the open chapter number
	 */
	if (has_sparse(volume_index)) {
		uds_lock_mutex(mutex);
		set_volume_sub_index_zone_open_chapter(&volume_index->vi_hook,
						       zone_number,
						       virtual_chapter);
		uds_unlock_mutex(mutex);
	}
}

/**
 * Set the open chapter number.  The volume index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param volume_index     The volume index
 * @param virtual_chapter  The new open chapter number
 **/
void set_volume_index_open_chapter(struct volume_index *volume_index,
				   uint64_t virtual_chapter)
{
	unsigned int zone;

	for (zone = 0; zone < volume_index->num_zones; zone++) {
		set_volume_index_zone_open_chapter(volume_index,
						   zone,
						   virtual_chapter);
	}
}

int set_volume_index_record_chapter(struct volume_index_record *record,
				    uint64_t virtual_chapter)
{
	const struct volume_sub_index *sub_index = record->sub_index;
	int result = validate_record(record);

	if (result != UDS_SUCCESS) {
		return result;
	}
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct volume_sub_index_zone *sub_index_zone =
			get_zone_for_record(record);
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot set chapter number %llu that is out of the valid range %llu to %llu",
						(unsigned long long) virtual_chapter,
						(unsigned long long) sub_index_zone->virtual_chapter_low,
						(unsigned long long) sub_index_zone->virtual_chapter_high);
	}
	if (unlikely(record->mutex != NULL)) {
		uds_lock_mutex(record->mutex);
	}
	result = set_delta_entry_value(&record->delta_entry,
				       convert_virtual_to_index(sub_index,
								virtual_chapter));
	if (unlikely(record->mutex != NULL)) {
		uds_unlock_mutex(record->mutex);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}
	record->virtual_chapter = virtual_chapter;
	return UDS_SUCCESS;
}

/**
 * Set the tag value used when saving and/or restoring a volume index.
 *
 * @param volume_index  The volume index
 * @param tag           The tag value
 **/
static void set_volume_index_tag(struct volume_sub_index *sub_index, byte tag)
{
	set_delta_index_tag(&sub_index->delta_index, tag);
}

/**
 * Do a quick read-only lookup of the sampled record name and return
 * information needed by the index code to process the record name.
 *
 * @param sub_index        The volume subindex
 * @param name             The record name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
static uint64_t
lookup_volume_sub_index_name(const struct volume_sub_index *sub_index,
			     const struct uds_record_name *name)
{
	int result;
	unsigned int address = extract_address(sub_index, name);
	unsigned int delta_list_number = extract_dlist_num(sub_index, name);
	unsigned int zone_number =
		get_volume_sub_index_zone(sub_index, name);
	const struct volume_sub_index_zone *zone = &sub_index->zones[zone_number];
	uint64_t virtual_chapter;
	unsigned int index_chapter;
	unsigned int rolling_chapter;
	struct delta_index_entry delta_entry;

	result = get_delta_index_entry(&sub_index->delta_index,
				       delta_list_number,
				       address,
				       name->name,
				       &delta_entry);
	if (result != UDS_SUCCESS) {
		return UINT64_MAX;
	}

	if (delta_entry.at_end || (delta_entry.key != address)) {
		return UINT64_MAX;
	}

	index_chapter = get_delta_entry_value(&delta_entry);
	rolling_chapter = ((index_chapter - zone->virtual_chapter_low) &
			   sub_index->chapter_mask);

	virtual_chapter = zone->virtual_chapter_low + rolling_chapter;
	if (virtual_chapter > zone->virtual_chapter_high) {
		return UINT64_MAX;
	}

	return virtual_chapter;
}

/**
 * Do a quick read-only lookup of the record name and return information
 * needed by the index code to process the record name.
 *
 * @param volume_index  The volume index
 * @param name          The record name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
uint64_t lookup_volume_index_name(const struct volume_index *volume_index,
				  const struct uds_record_name *name)
{
	unsigned int zone_number = get_volume_index_zone(volume_index, name);
	struct mutex *mutex = &volume_index->zones[zone_number].hook_mutex;
	uint64_t virtual_chapter;

	if (!is_volume_index_sample(volume_index, name)) {
		return UINT64_MAX;
	}

	uds_lock_mutex(mutex);
	virtual_chapter = lookup_volume_sub_index_name(&volume_index->vi_hook, name);
	uds_unlock_mutex(mutex);

        return virtual_chapter;
}

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
static void abort_restoring_volume_sub_index(struct volume_sub_index *sub_index)
{
	abort_restoring_delta_index(&sub_index->delta_index);
}

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
void abort_restoring_volume_index(struct volume_index *volume_index)
{
	abort_restoring_volume_sub_index(&volume_index->vi_non_hook);
	if (has_sparse(volume_index)) {
		abort_restoring_volume_sub_index(&volume_index->vi_hook);
	}
}

static int __must_check decode_volume_sub_index_header(struct buffer *buffer,
						       struct sub_index_data *header)
{
	int result = get_bytes_from_buffer(buffer, sizeof(header->magic),
					   &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &header->volume_nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer,
					   &header->virtual_chapter_low);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer,
					   &header->virtual_chapter_high);
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
	result = ASSERT(content_length(buffer) == 0,
			"%zu bytes decoded of %zu expected",
			buffer_length(buffer) - content_length(buffer),
			buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		result = UDS_CORRUPT_DATA;
	}

	return result;
}

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_restoring_volume_sub_index(struct volume_sub_index *sub_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int num_readers)
{
	unsigned int z;
	int result;
	uint64_t *first_flush_chapter;
	uint64_t virtual_chapter_low = 0, virtual_chapter_high = 0;
	unsigned int i;

	if (sub_index == NULL) {
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cannot restore to null volume index");
	}
	empty_delta_index(&sub_index->delta_index);

	for (i = 0; i < num_readers; i++) {
		struct buffer *buffer;
		struct sub_index_data header;
		int result = make_buffer(sizeof(struct sub_index_data), &buffer);

		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index header");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = decode_volume_sub_index_header(buffer, &header);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (memcmp(header.magic, MAGIC_START_5, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");
		}

		if (sub_index->volume_nonce == 0) {
			sub_index->volume_nonce = header.volume_nonce;
		} else if (header.volume_nonce != sub_index->volume_nonce) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index volume nonce incorrect");
		}

		if (i == 0) {
			virtual_chapter_low = header.virtual_chapter_low;
			virtual_chapter_high = header.virtual_chapter_high;
		} else if (virtual_chapter_high !=
			   header.virtual_chapter_high) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"Inconsistent volume index zone files: Chapter range is [%llu,%llu], chapter range %d is [%llu,%llu]",
							(unsigned long long) virtual_chapter_low,
							(unsigned long long) virtual_chapter_high,
							i,
							(unsigned long long) header.virtual_chapter_low,
							(unsigned long long) header.virtual_chapter_high);
		} else if (virtual_chapter_low < header.virtual_chapter_low) {
			virtual_chapter_low = header.virtual_chapter_low;
		}

		first_flush_chapter = &sub_index->flush_chapters[header.first_list];
		result = make_buffer(header.num_lists * sizeof(uint64_t),
				     &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index flush ranges");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = get_uint64_les_from_buffer(buffer, header.num_lists,
						    first_flush_chapter);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	for (z = 0; z < sub_index->num_zones; z++) {
		memset(&sub_index->zones[z],
		       0,
		       sizeof(struct volume_sub_index_zone));
		sub_index->zones[z].virtual_chapter_low = virtual_chapter_low;
		sub_index->zones[z].virtual_chapter_high = virtual_chapter_high;
	}

	result = start_restoring_delta_index(&sub_index->delta_index,
					     buffered_readers,
					     num_readers);
	if (result != UDS_SUCCESS) {
		return uds_log_warning_strerror(result,
						"restoring delta index failed");
	}
	return UDS_SUCCESS;
}

static int __must_check decode_volume_index_header(struct buffer *buffer,
						   struct volume_index_data *header)
{
	int result = get_bytes_from_buffer(buffer, sizeof(header->magic),
					   &header->magic);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		get_uint32_le_from_buffer(buffer, &header->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT(content_length(buffer) == 0,
			"%zu bytes decoded of %zu expected",
			buffer_length(buffer) - content_length(buffer),
			buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		result = UDS_CORRUPT_DATA;
	}
	return result;
}

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered reader to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int start_restoring_volume_index(struct volume_index *volume_index,
				 struct buffered_reader **buffered_readers,
				 unsigned int num_readers)
{
	unsigned int i;
	int result = ASSERT(volume_index != NULL,
			    "cannot restore to null volume index");
	if (result != UDS_SUCCESS) {
		return UDS_BAD_STATE;
	}

	if (!has_sparse(volume_index)) {
		return start_restoring_volume_sub_index(&volume_index->vi_non_hook,
							buffered_readers,
							num_readers);
	}

	for (i = 0; i < num_readers; i++) {
		struct volume_index_data header;
		struct buffer *buffer;

		result = make_buffer(sizeof(struct volume_index_data), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(buffered_readers[i],
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_warning_strerror(result,
							"failed to read volume index header");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = decode_volume_index_header(buffer, &header);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (memcmp(header.magic, MAGIC_START_6, MAGIC_SIZE) != 0) {
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");
		}

		if (i == 0) {
			volume_index->sparse_sample_rate = header.sparse_sample_rate;
		} else if (volume_index->sparse_sample_rate !=
			   header.sparse_sample_rate) {
			uds_log_warning_strerror(UDS_CORRUPT_DATA,
						 "Inconsistent sparse sample rate in delta index zone files: %u vs. %u",
						 volume_index->sparse_sample_rate,
						 header.sparse_sample_rate);
			return UDS_CORRUPT_DATA;
		}
	}

	result = start_restoring_volume_sub_index(&volume_index->vi_non_hook,
						  buffered_readers,
						  num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return start_restoring_volume_sub_index(&volume_index->vi_hook,
						buffered_readers,
						num_readers);
}

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param sub_index         The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
static int
finish_restoring_volume_sub_index(struct volume_sub_index *sub_index,
				  struct buffered_reader **buffered_readers,
				  unsigned int num_readers)
{
	return finish_restoring_delta_index(&sub_index->delta_index,
					    buffered_readers,
					    num_readers);
}

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
int finish_restoring_volume_index(struct volume_index *volume_index,
				  struct buffered_reader **buffered_readers,
				  unsigned int num_readers)
{
	int result;

	result = finish_restoring_volume_sub_index(&volume_index->vi_non_hook,
						   buffered_readers,
						   num_readers);
	if ((result == UDS_SUCCESS) && has_sparse(volume_index)) {
		result = finish_restoring_volume_sub_index(&volume_index->vi_hook,
							   buffered_readers,
							   num_readers);
	}
	return result;
}

int load_volume_index(struct volume_index *volume_index,
		      struct buffered_reader **readers,
		      unsigned int num_readers)
{
	/* Start by reading the "header" section of the stream */
	int result = start_restoring_volume_index(volume_index,
						  readers,
						  num_readers);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = finish_restoring_volume_index(volume_index,
					       readers,
					       num_readers);
	if (result != UDS_SUCCESS) {
		abort_restoring_volume_index(volume_index);
		return result;
	}

	/* Check the final guard lists to make sure we read everything. */
	result = check_guard_delta_lists(readers, num_readers);
	if (result != UDS_SUCCESS) {
		abort_restoring_volume_index(volume_index);
	}

	return result;
}

static int __must_check encode_volume_sub_index_header(struct buffer *buffer,
						       struct sub_index_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START_5);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, header->volume_nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result =
		put_uint64_le_into_buffer(buffer, header->virtual_chapter_low);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer,
					   header->virtual_chapter_high);
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
	return ASSERT(content_length(buffer) == sizeof(struct sub_index_data),
		      "%zu bytes of config written, of %zu expected",
		      content_length(buffer),
		      sizeof(struct sub_index_data));
}

/**
 * Start saving a volume index to a buffered output stream.
 *
 * @param volume_index     The volume index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
start_saving_volume_sub_index(const struct volume_sub_index *sub_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	int result;
	struct volume_sub_index_zone *volume_index_zone =
		&sub_index->zones[zone_number];
	unsigned int first_list =
		get_delta_zone_first_list(&sub_index->delta_index, zone_number);
	unsigned int num_lists =
		get_delta_zone_list_count(&sub_index->delta_index, zone_number);

	struct sub_index_data header;
	uint64_t *first_flush_chapter;
	struct buffer *buffer;

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_START_5, MAGIC_SIZE);
	header.volume_nonce = sub_index->volume_nonce;
	header.virtual_chapter_low = volume_index_zone->virtual_chapter_low;
	header.virtual_chapter_high = volume_index_zone->virtual_chapter_high;
	header.first_list = first_list;
	header.num_lists = num_lists;

	result = make_buffer(sizeof(struct sub_index_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_volume_sub_index_header(buffer, &header);
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
						"failed to write volume index header");
	}

	result = make_buffer(num_lists * sizeof(uint64_t), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	first_flush_chapter = &sub_index->flush_chapters[first_list];
	result = put_uint64_les_into_buffer(buffer, num_lists,
					    first_flush_chapter);
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
						"failed to write volume index flush ranges");
	}

	return start_saving_delta_index(&sub_index->delta_index, zone_number,
					buffered_writer);
}

static int __must_check encode_volume_index_header(struct buffer *buffer,
						   struct volume_index_data *header)
{
	int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START_6);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, header->sparse_sample_rate);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return ASSERT(content_length(buffer) == sizeof(struct volume_index_data),
		      "%zu bytes of config written, of %zu expected",
		      content_length(buffer),
		      sizeof(struct volume_index_data));
}

/**
 * Start saving a volume index to a buffered output stream.
 *
 * @param volume_index     The volume index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int start_saving_volume_index(const struct volume_index *volume_index,
			      unsigned int zone_number,
			      struct buffered_writer *buffered_writer)
{
	struct volume_index_data header;
	struct buffer *buffer;
	int result;

	if (!has_sparse(volume_index)) {
		return start_saving_volume_sub_index(&volume_index->vi_non_hook,
						     zone_number,
						     buffered_writer);
	}

	result = make_buffer(sizeof(struct volume_index_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_START_6, MAGIC_SIZE);
	header.sparse_sample_rate = volume_index->sparse_sample_rate;
	result = encode_volume_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		uds_log_warning_strerror(result,
					 "failed to write volume index header");
		return result;
	}

	result = start_saving_volume_sub_index(&volume_index->vi_non_hook,
					       zone_number,
					       buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = start_saving_volume_sub_index(&volume_index->vi_hook,
					       zone_number,
					       buffered_writer);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return UDS_SUCCESS;
}

/**
 * Finish saving a volume index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param sub_index     The volume index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
finish_saving_volume_sub_index(const struct volume_sub_index *sub_index,
			       unsigned int zone_number)
{
	return finish_saving_delta_index(&sub_index->delta_index, zone_number);
}

/**
 * Finish saving a volume index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param volume_index  The volume index
 * @param zone_number   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int finish_saving_volume_index(const struct volume_index *volume_index,
			       unsigned int zone_number)
{
	int result = finish_saving_volume_sub_index(&volume_index->vi_non_hook,
						    zone_number);

	if ((result == UDS_SUCCESS) && has_sparse(volume_index)) {
		result = finish_saving_volume_sub_index(&volume_index->vi_hook,
							zone_number);
	}
	return result;
}

int save_volume_index(struct volume_index *volume_index,
		      struct buffered_writer **writers,
		      unsigned int num_writers)
{
	int result = UDS_SUCCESS;
	unsigned int zone;

	for (zone = 0; zone < num_writers; ++zone) {
		result = start_saving_volume_index(volume_index,
						   zone,
						   writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = finish_saving_volume_index(volume_index, zone);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = write_guard_delta_list(writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}

		result = flush_buffered_writer(writers[zone]);
		if (result != UDS_SUCCESS) {
			break;
		}
	}

	return result;
}

/**
 * Return the volume subindex stats.
 *
 * @param sub_index     The volume subindex
 * @param stats         Stats for this subindex
 **/
static void get_volume_sub_index_stats(const struct volume_sub_index *sub_index,
				       struct volume_index_stats *stats)
{
	struct delta_index_stats dis;
	unsigned int z;

	get_delta_index_stats(&sub_index->delta_index, &dis);
	stats->memory_allocated =
		(dis.memory_allocated + sizeof(struct volume_sub_index) +
		 sub_index->num_delta_lists * sizeof(uint64_t) +
		 sub_index->num_zones * sizeof(struct volume_sub_index_zone));
	stats->rebalance_time = dis.rebalance_time;
	stats->rebalance_count = dis.rebalance_count;
	stats->record_count = dis.record_count;
	stats->collision_count = dis.collision_count;
	stats->discard_count = dis.discard_count;
	stats->overflow_count = dis.overflow_count;
	stats->num_lists = dis.list_count;
	stats->early_flushes = 0;
	for (z = 0; z < sub_index->num_zones; z++) {
		stats->early_flushes += sub_index->zones[z].num_early_flushes;
	}
}

/**
 * Return the volume index stats.  There is only one portion of the volume
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param volume_index  The volume index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
void get_volume_index_stats(const struct volume_index *volume_index,
			    struct volume_index_stats *dense,
			    struct volume_index_stats *sparse)
{
	get_volume_sub_index_stats(&volume_index->vi_non_hook, dense);
	if (has_sparse(volume_index)) {
		get_volume_sub_index_stats(&volume_index->vi_hook, sparse);
	} else {
		memset(sparse, 0, sizeof(struct volume_index_stats));
	}
}

#ifdef TEST_INTERNAL
void get_volume_index_combined_stats(const struct volume_index *volume_index,
				     struct volume_index_stats *stats)
{
	struct volume_index_stats dense, sparse;

	get_volume_index_stats(volume_index, &dense, &sparse);
	stats->memory_allocated =
		dense.memory_allocated + sparse.memory_allocated;
	stats->rebalance_time = dense.rebalance_time + sparse.rebalance_time;
	stats->rebalance_count =
		dense.rebalance_count + sparse.rebalance_count;
	stats->record_count = dense.record_count + sparse.record_count;
	stats->collision_count =
		dense.collision_count + sparse.collision_count;
	stats->discard_count = dense.discard_count + sparse.discard_count;
	stats->overflow_count = dense.overflow_count + sparse.overflow_count;
	stats->num_lists = dense.num_lists + sparse.num_lists;
	stats->early_flushes = dense.early_flushes + sparse.early_flushes;
}

#endif /* TEST_INTERNAL */
/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to authenticate the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
static int initialize_volume_sub_index(const struct configuration *config,
				       uint64_t volume_nonce,
				       struct volume_sub_index *sub_index)
{
	struct sub_index_parameters params = { .address_bits = 0 };
	unsigned int num_zones = config->zone_count;
	int result = compute_volume_index_parameters(config, &params);

	if (result != UDS_SUCCESS) {
		return result;
	}

	sub_index->address_bits = params.address_bits;
	sub_index->address_mask = (1u << params.address_bits) - 1;
	sub_index->chapter_bits = params.chapter_bits;
	sub_index->chapter_mask = (1u << params.chapter_bits) - 1;
	sub_index->num_chapters = params.num_chapters;
	sub_index->num_delta_lists = params.num_delta_lists;
	sub_index->num_zones = num_zones;
	sub_index->chapter_zone_bits = params.num_bits_per_chapter / num_zones;
	sub_index->volume_nonce = volume_nonce;

	result = initialize_delta_index(&sub_index->delta_index,
					num_zones,
					params.num_delta_lists,
					params.mean_delta,
					params.chapter_bits,
					params.memory_size);
	if (result != UDS_SUCCESS) {
		return result;
	}

	sub_index->max_zone_bits =
		((get_delta_index_bits_allocated(&sub_index->delta_index) -
			params.target_free_size * CHAR_BIT) / num_zones);

	/*
	 * Initialize the chapter flush ranges to be empty.  This depends upon
	 * allocate returning zeroed memory.
	 */
	result = UDS_ALLOCATE(params.num_delta_lists,
			      uint64_t,
			      "first chapter to flush",
			      &sub_index->flush_chapters);
	if (result != UDS_SUCCESS) {
		return result;
	}

	/*
	 * Initialize the virtual chapter ranges to start at zero.  This
	 * depends upon allocate returning zeroed memory.
	 */

	return UDS_ALLOCATE(num_zones,
			    struct volume_sub_index_zone,
			    "volume index zones",
			    &sub_index->zones);
}

/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to authenticate the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int make_volume_index(const struct configuration *config,
		      uint64_t volume_nonce,
		      struct volume_index **volume_index_ptr)
{
	struct split_config split;
	unsigned int zone;
	struct volume_index *volume_index;
	int result;

	result = UDS_ALLOCATE(1, struct volume_index, "volume index", &volume_index);
	if (result != UDS_SUCCESS) {
		return result;
	}

	volume_index->num_zones = config->zone_count;

	if (!uses_sparse(config)) {
		result = initialize_volume_sub_index(config,
						     volume_nonce,
						     &volume_index->vi_non_hook);
		if (result != UDS_SUCCESS) {
			free_volume_index(volume_index);
			return result;
                }

		*volume_index_ptr = volume_index;
		return UDS_SUCCESS;
	}

	result = split_configuration(config, &split);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return result;
	}

	volume_index->sparse_sample_rate = config->sparse_sample_rate;

	result = UDS_ALLOCATE(config->zone_count,
			      struct volume_index_zone,
			      "volume index zones",
			      &volume_index->zones);
	for (zone = 0; zone < config->zone_count; zone++) {
		if (result == UDS_SUCCESS) {
			result = uds_init_mutex(&volume_index->zones[zone].hook_mutex);
		}
	}
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return result;
	}

	result = initialize_volume_sub_index(&split.non_hook_config,
					     volume_nonce,
					     &volume_index->vi_non_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return uds_log_error_strerror(result,
					      "Error creating non hook volume index");
	}
	set_volume_index_tag(&volume_index->vi_non_hook, 'd');

	result = initialize_volume_sub_index(&split.hook_config,
					     volume_nonce,
					     &volume_index->vi_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return uds_log_error_strerror(result,
					      "Error creating hook volume index");
	}
	set_volume_index_tag(&volume_index->vi_hook, 's');

	*volume_index_ptr = volume_index;
	return UDS_SUCCESS;
}
