// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */
#include "volume-index.h"

#include <linux/cache.h>
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
 * The volume index is combination of two separate subindexes, one containing sparse hook entries
 * (retained for all chapters), and one containing the remaining (non-hook) entries (retained only
 * for the dense chapters). If there are no sparse chapters, only the non-hook sub index is used.
 *
 * The volume index is also divided into zones, with one thread operating on each zone. Each
 * incoming request is dispatched to the appropriate thread, and then to the appropriate subindex.
 * Each delta list is handled by a single zone. To ensure that the distribution of delta lists to
 * zones doesn't underflow (leaving some zone with no delta lists), the minimum number of delta
 * lists must be the square of the maximum zone count for both subindexes.
 *
 * Most operations that use all the zones take place either before request processing is allowed,
 * or after all requests have been flushed in order to shut down. The only multi-threaded operation
 * supported during normal operation is the lookup_volume_index_name() method, used to determine
 * whether a new chapter should be loaded into the sparse index cache. This operation only uses the
 * sparse hook subindex, and the zone mutexes are used to make this operation safe.
 *
 * Each subindex is a delta index where the payload is a chapter number. The volume index knows how
 * to compute the delta list number and address from a record name.
 *
 * There are three ways of expressing chapter numbers in the volume index: virtual, index, and
 * rolling. The interface to the volume index uses virtual chapter numbers, which are 64 bits long.
 * Internally the subindex stores only the minimal number of bits necessary by masking away the
 * high-order bits.
 *
 * When we need to deal with ordering of index chapter numbers, as when flushing entries from older
 * chapters, we roll the index chapter number around so that the smallest one we are using has the
 * representation 0. See convert_index_to_virtual() or flush_invalid_entries() for an example of
 * this technique.
 *
 * For efficiency, when older chapter numbers become invalid, the index does not immediately remove
 * the invalidated entries. Instead it lazily removes them from a given delta list the next time it
 * walks that list during normal operation. Because of this, the index size must be increased
 * somewhat to accommodate all the invalid entries that have not yet been removed. For the standard
 * index sizes, this requires about 4 chapters of old entries per 1024 chapters of valid entries in
 * the index.
 */

struct sub_index_parameters {
	/* The number of bits in address mask */
	unsigned int address_bits;
	/* The number of bits in chapter number */
	unsigned int chapter_bits;
	/* The mean delta */
	unsigned int mean_delta;
	/* The number of delta lists */
	unsigned long num_delta_lists;
	/* The number of chapters used */
	unsigned long num_chapters;
	/* The number of bits per chapter */
	size_t num_bits_per_chapter;
	/* The number of bytes of delta list memory */
	size_t memory_size;
	/* The number of free bytes we desire */
	size_t target_free_size;
};

struct split_config {
	/* The hook subindex configuration */
	struct configuration hook_config;
	struct geometry hook_geometry;

	/* The non-hook subindex configuration */
	struct configuration non_hook_config;
	struct geometry non_hook_geometry;
};

struct volume_sub_index_zone {
	u64 virtual_chapter_low;
	u64 virtual_chapter_high;
	long num_early_flushes;
} __aligned(L1_CACHE_BYTES);

struct volume_sub_index {
	/* The delta index */
	struct delta_index delta_index;
	/* The first chapter to be flushed in each zone */
	u64 *flush_chapters;
	/* The zones */
	struct volume_sub_index_zone *zones;
	/* The volume nonce */
	u64 volume_nonce;
	/* Expected size of a chapter (per zone) */
	u64 chapter_zone_bits;
	/* Maximum size of the index (per zone) */
	u64 max_zone_bits;
	/* The number of bits in address mask */
	unsigned int address_bits;
	/* Mask to get address within delta list */
	unsigned int address_mask;
	/* The number of bits in chapter number */
	unsigned int chapter_bits;
	/* The largest storable chapter number */
	unsigned int chapter_mask;
	/* The number of chapters used */
	unsigned int num_chapters;
	/* The number of delta lists */
	unsigned int num_delta_lists;
	/* The number of zones */
	unsigned int num_zones;
};

struct volume_index_zone {
	/* Protects the sampled index in this zone */
	struct mutex hook_mutex;
} __aligned(L1_CACHE_BYTES);

struct volume_index {
	unsigned int sparse_sample_rate;
	unsigned int num_zones;
	struct volume_sub_index vi_non_hook;
	struct volume_sub_index vi_hook;
	struct volume_index_zone *zones;
};

struct chapter_range {
	unsigned int chapter_start;
	unsigned int chapter_count;
};

enum { MAGIC_SIZE = 8 };
static const char MAGIC_START_5[] = "MI5-0005";

struct sub_index_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START_5 */
	u64 volume_nonce;
	u64 virtual_chapter_low;
	u64 virtual_chapter_high;
	unsigned int first_list;
	unsigned int num_lists;
};

static const char MAGIC_START_6[] = "MI6-0001";

struct volume_index_data {
	char magic[MAGIC_SIZE]; /* MAGIC_START_6 */
	unsigned int sparse_sample_rate;
};

static const u8 volume_index_record_magic = 0xAA;
static const u8 bad_magic;

#ifdef TEST_INTERNAL
/*
 * In production, the default value for min_volume_index_delta_lists will be MAX_ZONES * MAX_ZONES.
 * However, some unit tests will use only a single delta list for simplicity.
 */
unsigned int min_volume_index_delta_lists;

#endif /* TEST_INTERNAL */
static inline unsigned int
extract_address(const struct volume_sub_index *sub_index, const struct uds_record_name *name)
{
	return extract_volume_index_bytes(name) & sub_index->address_mask;
}

static inline unsigned int
extract_dlist_num(const struct volume_sub_index *sub_index, const struct uds_record_name *name)
{
	u64 bits = extract_volume_index_bytes(name);

	return (bits >> sub_index->address_bits) % sub_index->num_delta_lists;
}

static inline const struct volume_sub_index_zone *
get_zone_for_record(const struct volume_index_record *record)
{
	return &record->sub_index->zones[record->zone_number];
}

static inline u64
convert_index_to_virtual(const struct volume_index_record *record, unsigned int index_chapter)
{
	const struct volume_sub_index_zone *volume_index_zone = get_zone_for_record(record);
	unsigned int rolling_chapter = ((index_chapter - volume_index_zone->virtual_chapter_low) &
					record->sub_index->chapter_mask);

	return volume_index_zone->virtual_chapter_low + rolling_chapter;
}

static inline unsigned int
convert_virtual_to_index(const struct volume_sub_index *sub_index, u64 virtual_chapter)
{
	return virtual_chapter & sub_index->chapter_mask;
}

static inline bool
is_virtual_chapter_indexed(const struct volume_index_record *record, u64 virtual_chapter)
{
	const struct volume_sub_index_zone *volume_index_zone = get_zone_for_record(record);

	return (virtual_chapter >= volume_index_zone->virtual_chapter_low) &&
	       (virtual_chapter <= volume_index_zone->virtual_chapter_high);
}

static inline bool has_sparse(const struct volume_index *volume_index)
{
	return volume_index->sparse_sample_rate > 0;
}

bool is_volume_index_sample(const struct volume_index *volume_index,
			    const struct uds_record_name *name)
{
	if (!has_sparse(volume_index))
		return false;

	return (extract_sampling_bytes(name) % volume_index->sparse_sample_rate) == 0;
}

static inline const struct volume_sub_index *
get_sub_index(const struct volume_index *volume_index, const struct uds_record_name *name)
{
	return is_volume_index_sample(volume_index, name) ?
			&volume_index->vi_hook :
			&volume_index->vi_non_hook;
}

static unsigned int get_volume_sub_index_zone(const struct volume_sub_index *sub_index,
					      const struct uds_record_name *name)
{
	return get_delta_zone_number(&sub_index->delta_index, extract_dlist_num(sub_index, name));
}

unsigned int get_volume_index_zone(const struct volume_index *volume_index,
				   const struct uds_record_name *name)
{
	return get_volume_sub_index_zone(get_sub_index(volume_index, name), name);
}

static inline bool uses_sparse(const struct configuration *config)
{
	return is_sparse_geometry(config->geometry);
}

static int compute_volume_index_parameters(const struct configuration *config,
					   struct sub_index_parameters *params)
{
	enum { DELTA_LIST_SIZE = 256 };
	unsigned long invalid_chapters, address_span;
	unsigned long chapters_in_volume_index, entries_in_volume_index;
	unsigned long rounded_chapters;
	unsigned long delta_list_records;
	unsigned int num_addresses;
	u64 num_bits_per_index;
	size_t expected_index_size;
	unsigned long min_delta_lists = MAX_ZONES * MAX_ZONES;
	struct geometry *geometry = config->geometry;
	unsigned long records_per_chapter = geometry->records_per_chapter;

#ifdef TEST_INTERNAL
	if (min_volume_index_delta_lists > 0)
		min_delta_lists = min_volume_index_delta_lists;
#endif /* TEST_INTERNAL */
	params->num_chapters = geometry->chapters_per_volume;
	/*
	 * Make sure that the number of delta list records in the volume index does not change when
	 * the volume is reduced by one chapter. This preserves the mapping from name to volume
	 * index delta list.
	 */
	rounded_chapters = params->num_chapters;
	if (is_reduced_geometry(geometry))
		rounded_chapters += 1;
	delta_list_records = records_per_chapter * rounded_chapters;
	num_addresses = config->volume_index_mean_delta * DELTA_LIST_SIZE;
	params->num_delta_lists = max(delta_list_records / DELTA_LIST_SIZE, min_delta_lists);
	params->address_bits = bits_per(num_addresses - 1);
	params->chapter_bits = bits_per(rounded_chapters - 1);
	if ((unsigned int) params->num_delta_lists != params->num_delta_lists)
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu delta lists",
						params->num_delta_lists);
	if (params->address_bits > 31)
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %u address bits",
						params->address_bits);
	if (is_sparse_geometry(geometry))
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize dense volume index with %u sparse chapters",
						geometry->sparse_chapters_per_volume);
	if (records_per_chapter == 0)
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu records per chapter",
						records_per_chapter);
	if (params->num_chapters == 0)
		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot initialize volume index with %lu chapters per volume",
						params->num_chapters);

	/*
	 * The probability that a given delta list is not touched during the writing of an entire
	 * chapter is:
	 *
	 * double p_not_touched = pow((double) (params->num_delta_lists - 1)
	 *			    / params->num_delta_lists,
	 *			    records_per_chapter);
	 *
	 * For the standard index sizes, about 78% of the delta lists are not touched, and
	 * therefore contain old index entries that have not been eliminated by the lazy LRU
	 * processing. Then the number of old index entries that accumulate over the entire index,
	 * in terms of full chapters worth of entries, is:
	 *
	 * double invalid_chapters = p_not_touched / (1.0 - p_not_touched);
	 *
	 * For the standard index sizes, the index needs about 3.5 chapters of space for the old
	 * entries in a 1024 chapter index, so round this up to use 4 chapters per 1024 chapters in
	 * the index.
	 */
	invalid_chapters = max(rounded_chapters / 256, 2UL);
	chapters_in_volume_index = rounded_chapters + invalid_chapters;
	entries_in_volume_index = records_per_chapter * chapters_in_volume_index;

	address_span = (u64) params->num_delta_lists << params->address_bits;
	params->mean_delta = address_span / entries_in_volume_index;

	/*
	 * Compute the expected size of a full index, then set the total memory to be 6% larger
	 * than that expected size. This number should be large enough that there are not many
	 * rebalances when the index is full.
	 */
	params->num_bits_per_chapter = compute_delta_index_size(records_per_chapter,
								params->mean_delta,
								params->chapter_bits);
	num_bits_per_index = params->num_bits_per_chapter * chapters_in_volume_index;
	expected_index_size = num_bits_per_index / BITS_PER_BYTE;
	params->memory_size = expected_index_size * 106 / 100;

	params->target_free_size = expected_index_size / 20;
	return UDS_SUCCESS;
}

static void uninitialize_volume_sub_index(struct volume_sub_index *sub_index)
{
	UDS_FREE(sub_index->flush_chapters);
	sub_index->flush_chapters = NULL;
	UDS_FREE(sub_index->zones);
	sub_index->zones = NULL;
	uninitialize_delta_index(&sub_index->delta_index);
}

void free_volume_index(struct volume_index *volume_index)
{
	if (volume_index == NULL)
		return;

	if (volume_index->zones != NULL) {
		unsigned int zone;

		for (zone = 0; zone < volume_index->num_zones; zone++)
			uds_destroy_mutex(&volume_index->zones[zone].hook_mutex);
		UDS_FREE(volume_index->zones);
		volume_index->zones = NULL;
	}
	uninitialize_volume_sub_index(&volume_index->vi_non_hook);
	uninitialize_volume_sub_index(&volume_index->vi_hook);
	UDS_FREE(volume_index);
}


static int
compute_volume_sub_index_save_bytes(const struct configuration *config, size_t *num_bytes)
{
	struct sub_index_parameters params = { .address_bits = 0 };
	int result;

	result = compute_volume_index_parameters(config, &params);
	if (result != UDS_SUCCESS)
		return result;

	*num_bytes = (sizeof(struct sub_index_data) +
		      params.num_delta_lists * sizeof(u64) +
		      compute_delta_index_save_bytes(params.num_delta_lists, params.memory_size));
	return UDS_SUCCESS;
}

static int split_configuration(const struct configuration *config, struct split_config *split)
{
	u64 sample_rate, num_chapters, num_sparse_chapters;
	u64 num_dense_chapters, sample_records;
	int result;

	result = ASSERT(config->geometry->sparse_chapters_per_volume != 0,
			    "cannot initialize sparse+dense volume index with no sparse chapters");
	if (result != UDS_SUCCESS)
		return UDS_INVALID_ARGUMENT;
	result = ASSERT(config->sparse_sample_rate != 0,
			"cannot initialize sparse+dense volume index with a sparse sample rate of %u",
			config->sparse_sample_rate);
	if (result != UDS_SUCCESS)
		return UDS_INVALID_ARGUMENT;

	/* Start with copies of the base configuration. */
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

	/* Adjust the number of records indexed for each chapter. */
	split->hook_geometry.records_per_chapter = sample_records;
	split->non_hook_geometry.records_per_chapter -= sample_records;

	/* Adjust the number of chapters indexed. */
	split->hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.sparse_chapters_per_volume = 0;
	split->non_hook_geometry.chapters_per_volume = num_dense_chapters;
	return UDS_SUCCESS;
}

static int compute_volume_index_save_bytes(const struct configuration *config, size_t *num_bytes)
{
	size_t hook_bytes, non_hook_bytes;
	struct split_config split;
	int result;

	result = split_configuration(config, &split);
	if (result != UDS_SUCCESS)
		return result;
	result = compute_volume_sub_index_save_bytes(&split.hook_config, &hook_bytes);
	if (result != UDS_SUCCESS)
		return result;
	result = compute_volume_sub_index_save_bytes(&split.non_hook_config, &non_hook_bytes);
	if (result != UDS_SUCCESS)
		return result;

	*num_bytes = sizeof(struct volume_index_data) + hook_bytes + non_hook_bytes;
	return UDS_SUCCESS;
}

int compute_volume_index_save_blocks(const struct configuration *config,
				     size_t block_size,
				     u64 *block_count)
{
	size_t num_bytes;
	int result;

	result = (uses_sparse(config) ? compute_volume_index_save_bytes(config, &num_bytes) :
					compute_volume_sub_index_save_bytes(config, &num_bytes));
	if (result != UDS_SUCCESS)
		return result;

	num_bytes += sizeof(struct delta_list_save_info);
	*block_count = DIV_ROUND_UP(num_bytes, block_size) + MAX_ZONES;
	return UDS_SUCCESS;
}

#ifdef TEST_INTERNAL
static size_t
get_volume_sub_index_memory_used(const struct volume_sub_index *sub_index)
{
	return DIV_ROUND_UP(get_delta_index_bits_used(&sub_index->delta_index), BITS_PER_BYTE);
}

size_t get_volume_index_memory_used(const struct volume_index *volume_index)
{
	size_t memory;

	memory = get_volume_sub_index_memory_used(&volume_index->vi_non_hook);
	if (has_sparse(volume_index))
		memory += get_volume_sub_index_memory_used(&volume_index->vi_hook);

	return memory;
}

#endif /* TEST_INTERNAL */
/* Flush invalid entries while walking the delta list. */
static inline int flush_invalid_entries(struct volume_index_record *record,
					struct chapter_range *flush_range,
					unsigned int *next_chapter_to_invalidate)
{
	int result;

	result = next_delta_index_entry(&record->delta_entry);
	if (result != UDS_SUCCESS)
		return result;
	while (!record->delta_entry.at_end) {
		unsigned int index_chapter = get_delta_entry_value(&record->delta_entry);
		unsigned int relative_chapter = ((index_chapter - flush_range->chapter_start) &
						 record->sub_index->chapter_mask);
		if (likely(relative_chapter >= flush_range->chapter_count)) {
			if (relative_chapter < *next_chapter_to_invalidate)
				*next_chapter_to_invalidate = relative_chapter;
			break;
		}
		result = remove_delta_index_entry(&record->delta_entry);
		if (result != UDS_SUCCESS)
			return result;
	}
	return UDS_SUCCESS;
}

/* Find the matching record, or the list offset where the record would go. */
static int get_volume_index_entry(struct volume_index_record *record,
				  unsigned int list_number,
				  unsigned int key,
				  struct chapter_range *flush_range)
{
	struct volume_index_record other_record;
	const struct volume_sub_index *sub_index = record->sub_index;
	unsigned int next_chapter_to_invalidate = sub_index->chapter_mask;
	int result;

	result = start_delta_index_search(&sub_index->delta_index, list_number,
					  0, &record->delta_entry);
	if (result != UDS_SUCCESS)
		return result;
	do {
		result = flush_invalid_entries(record, flush_range, &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS)
			return result;
	} while (!record->delta_entry.at_end && (key > record->delta_entry.key));

	result = remember_delta_index_offset(&record->delta_entry);
	if (result != UDS_SUCCESS)
		return result;

	/* Check any collision records for a more precise match. */
	other_record = *record;
	if (!other_record.delta_entry.at_end && (key == other_record.delta_entry.key)) {
		for (;;) {
			u8 collision_name[UDS_RECORD_NAME_SIZE];

			result = flush_invalid_entries(&other_record,
						       flush_range,
						       &next_chapter_to_invalidate);
			if (result != UDS_SUCCESS)
				return result;
			if (other_record.delta_entry.at_end ||
			    !other_record.delta_entry.is_collision)
				break;
			result = get_delta_entry_collision(&other_record.delta_entry,
							   collision_name);
			if (result != UDS_SUCCESS)
				return result;
			if (memcmp(collision_name, record->name, UDS_RECORD_NAME_SIZE) == 0) {
				*record = other_record;
				break;
			}
		}
	}
	while (!other_record.delta_entry.at_end) {
		result = flush_invalid_entries(&other_record,
					       flush_range,
					       &next_chapter_to_invalidate);
		if (result != UDS_SUCCESS)
			return result;
	}
	next_chapter_to_invalidate += flush_range->chapter_start;
	next_chapter_to_invalidate &= sub_index->chapter_mask;
	flush_range->chapter_start = next_chapter_to_invalidate;
	flush_range->chapter_count = 0;
	return UDS_SUCCESS;
}

static int get_volume_sub_index_record(struct volume_sub_index *sub_index,
				       const struct uds_record_name *name,
				       struct volume_index_record *record)
{
	int result;
	const struct volume_sub_index_zone *volume_index_zone;
	unsigned int address = extract_address(sub_index, name);
	unsigned int delta_list_number = extract_dlist_num(sub_index, name);
	u64 flush_chapter = sub_index->flush_chapters[delta_list_number];

	record->magic = volume_index_record_magic;
	record->sub_index = sub_index;
	record->mutex = NULL;
	record->name = name;
	record->zone_number = get_delta_zone_number(&sub_index->delta_index, delta_list_number);
	volume_index_zone = get_zone_for_record(record);

	if (flush_chapter < volume_index_zone->virtual_chapter_low) {
		struct chapter_range range;
		u64 flush_count = volume_index_zone->virtual_chapter_low - flush_chapter;

		range.chapter_start = convert_virtual_to_index(sub_index, flush_chapter);
		range.chapter_count = (flush_count > sub_index->chapter_mask ?
				       sub_index->chapter_mask + 1 :
				       flush_count);
		result = get_volume_index_entry(record, delta_list_number, address, &range);
		flush_chapter = convert_index_to_virtual(record, range.chapter_start);
		if (flush_chapter > volume_index_zone->virtual_chapter_high)
			flush_chapter = volume_index_zone->virtual_chapter_high;
		sub_index->flush_chapters[delta_list_number] = flush_chapter;
	} else {
		result = get_delta_index_entry(&sub_index->delta_index,
					       delta_list_number,
					       address,
					       name->name,
					       &record->delta_entry);
	}
	if (result != UDS_SUCCESS)
		return result;
	record->is_found = (!record->delta_entry.at_end && (record->delta_entry.key == address));
	if (record->is_found) {
		unsigned int index_chapter = get_delta_entry_value(&record->delta_entry);

		record->virtual_chapter = convert_index_to_virtual(record, index_chapter);
	}
	record->is_collision = record->delta_entry.is_collision;
	return UDS_SUCCESS;
}

int get_volume_index_record(struct volume_index *volume_index,
			    const struct uds_record_name *name,
			    struct volume_index_record *record)
{
	int result;

	if (is_volume_index_sample(volume_index, name)) {
		/*
		 * We need to prevent a lookup_volume_index_name() happening while we are finding
		 * the volume index record. Remember that because of lazy LRU flushing of the
		 * volume index, get_volume_index_record() is not a read-only operation.
		 */
		unsigned int zone = get_volume_sub_index_zone(&volume_index->vi_hook, name);
		struct mutex *mutex = &volume_index->zones[zone].hook_mutex;

		uds_lock_mutex(mutex);
		result = get_volume_sub_index_record(&volume_index->vi_hook, name, record);
		uds_unlock_mutex(mutex);
		/* Remember the mutex so that other operations on the index record can use it. */
		record->mutex = mutex;
	} else {
		result = get_volume_sub_index_record(&volume_index->vi_non_hook, name, record);
	}
	return result;
}

int put_volume_index_record(struct volume_index_record *record, u64 virtual_chapter)
{
	int result;
	unsigned int address;
	const struct volume_sub_index *sub_index = record->sub_index;

	if (record->magic != volume_index_record_magic)
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
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
	if (unlikely(record->mutex != NULL))
		uds_lock_mutex(record->mutex);
	result = put_delta_index_entry(&record->delta_entry,
				       address,
				       convert_virtual_to_index(sub_index, virtual_chapter),
				       record->is_found ? record->name->name : NULL);
	if (unlikely(record->mutex != NULL))
		uds_unlock_mutex(record->mutex);
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

static inline int validate_record(struct volume_index_record *record)
{
	if (record->magic != volume_index_record_magic)
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"bad magic number in volume index record");
	if (!record->is_found)
		return uds_log_warning_strerror(UDS_BAD_STATE, "illegal operation on new record");
	return UDS_SUCCESS;
}

int remove_volume_index_record(struct volume_index_record *record)
{
	int result;

	result = validate_record(record);
	if (result != UDS_SUCCESS)
		return result;
	/* Mark the record so that it cannot be used again */
	record->magic = bad_magic;
	if (unlikely(record->mutex != NULL))
		uds_lock_mutex(record->mutex);
	result = remove_delta_index_entry(&record->delta_entry);
	if (unlikely(record->mutex != NULL))
		uds_unlock_mutex(record->mutex);
	return result;
}

static void set_volume_sub_index_zone_open_chapter(struct volume_sub_index *sub_index,
						   unsigned int zone_number,
						   u64 virtual_chapter)
{
	u64 used_bits;
	struct volume_sub_index_zone *zone = &sub_index->zones[zone_number];

	zone->virtual_chapter_low = (virtual_chapter >= sub_index->num_chapters ?
				     virtual_chapter - sub_index->num_chapters + 1 :
				     0);
	zone->virtual_chapter_high = virtual_chapter;

	/* Check to see if the zone data has grown to be too large. */
	used_bits = get_delta_zone_bits_used(&sub_index->delta_index, zone_number);
	if (used_bits > sub_index->max_zone_bits) {
		/* Expire enough chapters to free the desired space. */
		u64 expire_count =
			1 + (used_bits - sub_index->max_zone_bits) / sub_index->chapter_zone_bits;

		if (expire_count == 1) {
			uds_log_ratelimit(uds_log_info,
					  "zone %u:  At chapter %llu, expiring chapter %llu early",
					  zone_number,
					  (unsigned long long) virtual_chapter,
					  (unsigned long long) zone->virtual_chapter_low);
			zone->num_early_flushes++;
			zone->virtual_chapter_low++;
		} else {
			u64 first_expired = zone->virtual_chapter_low;

			if (first_expired + expire_count < zone->virtual_chapter_high) {
				zone->num_early_flushes += expire_count;
				zone->virtual_chapter_low += expire_count;
			} else {
				zone->num_early_flushes +=
					zone->virtual_chapter_high - zone->virtual_chapter_low;
				zone->virtual_chapter_low = zone->virtual_chapter_high;
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
					u64 virtual_chapter)
{
	struct mutex *mutex = &volume_index->zones[zone_number].hook_mutex;

	set_volume_sub_index_zone_open_chapter(&volume_index->vi_non_hook,
					       zone_number,
					       virtual_chapter);

	/*
	 * We need to prevent calling lookup_volume_index_name() while we are changing the open
	 * chapter number.
	 */
	if (has_sparse(volume_index)) {
		uds_lock_mutex(mutex);
		set_volume_sub_index_zone_open_chapter(&volume_index->vi_hook,
						       zone_number,
						       virtual_chapter);
		uds_unlock_mutex(mutex);
	}
}

/*
 * Set the newest open chapter number for the index, while also advancing the oldest valid chapter
 * number.
 */
void set_volume_index_open_chapter(struct volume_index *volume_index, u64 virtual_chapter)
{
	unsigned int zone;

	for (zone = 0; zone < volume_index->num_zones; zone++)
		set_volume_index_zone_open_chapter(volume_index, zone, virtual_chapter);
}

int set_volume_index_record_chapter(struct volume_index_record *record, u64 virtual_chapter)
{
	const struct volume_sub_index *sub_index = record->sub_index;
	int result;

	result = validate_record(record);
	if (result != UDS_SUCCESS)
		return result;
	if (!is_virtual_chapter_indexed(record, virtual_chapter)) {
		const struct volume_sub_index_zone *sub_index_zone = get_zone_for_record(record);

		return uds_log_warning_strerror(UDS_INVALID_ARGUMENT,
						"cannot set chapter number %llu that is out of the valid range %llu to %llu",
						(unsigned long long) virtual_chapter,
						(unsigned long long) sub_index_zone->virtual_chapter_low,
						(unsigned long long) sub_index_zone->virtual_chapter_high);
	}
	if (unlikely(record->mutex != NULL))
		uds_lock_mutex(record->mutex);
	result = set_delta_entry_value(&record->delta_entry,
				       convert_virtual_to_index(sub_index, virtual_chapter));
	if (unlikely(record->mutex != NULL))
		uds_unlock_mutex(record->mutex);
	if (result != UDS_SUCCESS)
		return result;
	record->virtual_chapter = virtual_chapter;
	return UDS_SUCCESS;
}

static void set_volume_index_tag(struct volume_sub_index *sub_index, u8 tag)
{
	set_delta_index_tag(&sub_index->delta_index, tag);
}

static u64 lookup_volume_sub_index_name(const struct volume_sub_index *sub_index,
					const struct uds_record_name *name)
{
	int result;
	unsigned int address = extract_address(sub_index, name);
	unsigned int delta_list_number = extract_dlist_num(sub_index, name);
	unsigned int zone_number = get_volume_sub_index_zone(sub_index, name);
	const struct volume_sub_index_zone *zone = &sub_index->zones[zone_number];
	u64 virtual_chapter;
	unsigned int index_chapter;
	unsigned int rolling_chapter;
	struct delta_index_entry delta_entry;

	result = get_delta_index_entry(&sub_index->delta_index,
				       delta_list_number,
				       address,
				       name->name,
				       &delta_entry);
	if (result != UDS_SUCCESS)
		return U64_MAX;

	if (delta_entry.at_end || (delta_entry.key != address))
		return U64_MAX;

	index_chapter = get_delta_entry_value(&delta_entry);
	rolling_chapter = ((index_chapter - zone->virtual_chapter_low) & sub_index->chapter_mask);

	virtual_chapter = zone->virtual_chapter_low + rolling_chapter;
	if (virtual_chapter > zone->virtual_chapter_high)
		return U64_MAX;

	return virtual_chapter;
}

/* Do a read-only lookup of the record name for sparse cache management. */
u64 lookup_volume_index_name(const struct volume_index *volume_index,
			     const struct uds_record_name *name)
{
	unsigned int zone_number = get_volume_index_zone(volume_index, name);
	struct mutex *mutex = &volume_index->zones[zone_number].hook_mutex;
	u64 virtual_chapter;

	if (!is_volume_index_sample(volume_index, name))
		return U64_MAX;

	uds_lock_mutex(mutex);
	virtual_chapter = lookup_volume_sub_index_name(&volume_index->vi_hook, name);
	uds_unlock_mutex(mutex);

	return virtual_chapter;
}

static void abort_restoring_volume_sub_index(struct volume_sub_index *sub_index)
{
	abort_restoring_delta_index(&sub_index->delta_index);
}

static void abort_restoring_volume_index(struct volume_index *volume_index)
{
	abort_restoring_volume_sub_index(&volume_index->vi_non_hook);
	if (has_sparse(volume_index))
		abort_restoring_volume_sub_index(&volume_index->vi_hook);
}

static int __must_check
decode_volume_sub_index_header(struct buffer *buffer, struct sub_index_data *header)
{
	int result;

	result = get_bytes_from_buffer(buffer, sizeof(header->magic), &header->magic);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u64_le_from_buffer(buffer, &header->volume_nonce);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u64_le_from_buffer(buffer, &header->virtual_chapter_low);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u64_le_from_buffer(buffer, &header->virtual_chapter_high);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u32_le_from_buffer(buffer, &header->first_list);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u32_le_from_buffer(buffer, &header->num_lists);
	if (result != UDS_SUCCESS)
		return result;
	result = ASSERT(content_length(buffer) == 0,
			"%zu bytes decoded of %zu expected",
			buffer_length(buffer) - content_length(buffer),
			buffer_length(buffer));
	if (result != UDS_SUCCESS)
		result = UDS_CORRUPT_DATA;

	return result;
}

static int start_restoring_volume_sub_index(struct volume_sub_index *sub_index,
					    struct buffered_reader **buffered_readers,
					    unsigned int num_readers)
{
	unsigned int z;
	int result;
	u64 *first_flush_chapter;
	u64 virtual_chapter_low = 0, virtual_chapter_high = 0;
	unsigned int i;

	if (sub_index == NULL)
		return uds_log_warning_strerror(UDS_BAD_STATE,
						"cannot restore to null volume index");
	empty_delta_index(&sub_index->delta_index);

	for (i = 0; i < num_readers; i++) {
		struct buffer *buffer;
		struct sub_index_data header;

		result = make_buffer(sizeof(struct sub_index_data), &buffer);
		if (result != UDS_SUCCESS)
			return result;

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
		if (result != UDS_SUCCESS)
			return result;

		if (memcmp(header.magic, MAGIC_START_5, MAGIC_SIZE) != 0)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");

		if (sub_index->volume_nonce == 0)
			sub_index->volume_nonce = header.volume_nonce;
		else if (header.volume_nonce != sub_index->volume_nonce)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index volume nonce incorrect");

		if (i == 0) {
			virtual_chapter_low = header.virtual_chapter_low;
			virtual_chapter_high = header.virtual_chapter_high;
		} else if (virtual_chapter_high != header.virtual_chapter_high) {
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
		result = make_buffer(header.num_lists * sizeof(u64), &buffer);
		if (result != UDS_SUCCESS)
			return result;

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

		result = get_u64_les_from_buffer(buffer, header.num_lists,
						 first_flush_chapter);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS)
			return result;
	}

	for (z = 0; z < sub_index->num_zones; z++) {
		memset(&sub_index->zones[z], 0, sizeof(struct volume_sub_index_zone));
		sub_index->zones[z].virtual_chapter_low = virtual_chapter_low;
		sub_index->zones[z].virtual_chapter_high = virtual_chapter_high;
	}

	result = start_restoring_delta_index(&sub_index->delta_index,
					     buffered_readers,
					     num_readers);
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result, "restoring delta index failed");
	return UDS_SUCCESS;
}

static int __must_check
decode_volume_index_header(struct buffer *buffer, struct volume_index_data *header)
{
	int result;

	result = get_bytes_from_buffer(buffer, sizeof(header->magic), &header->magic);
	if (result != UDS_SUCCESS)
		return result;
	result = get_u32_le_from_buffer(buffer, &header->sparse_sample_rate);
	if (result != UDS_SUCCESS)
		return result;
	result = ASSERT(content_length(buffer) == 0,
			"%zu bytes decoded of %zu expected",
			buffer_length(buffer) - content_length(buffer),
			buffer_length(buffer));
	if (result != UDS_SUCCESS)
		result = UDS_CORRUPT_DATA;
	return result;
}

static int start_restoring_volume_index(struct volume_index *volume_index,
					struct buffered_reader **buffered_readers,
					unsigned int num_readers)
{
	unsigned int i;
	int result;

	result = ASSERT(volume_index != NULL, "cannot restore to null volume index");
	if (result != UDS_SUCCESS)
		return UDS_BAD_STATE;

	if (!has_sparse(volume_index))
		return start_restoring_volume_sub_index(&volume_index->vi_non_hook,
							buffered_readers,
							num_readers);

	for (i = 0; i < num_readers; i++) {
		struct volume_index_data header;
		struct buffer *buffer;

		result = make_buffer(sizeof(struct volume_index_data), &buffer);
		if (result != UDS_SUCCESS)
			return result;

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
		if (result != UDS_SUCCESS)
			return result;

		if (memcmp(header.magic, MAGIC_START_6, MAGIC_SIZE) != 0)
			return uds_log_warning_strerror(UDS_CORRUPT_DATA,
							"volume index file had bad magic number");

		if (i == 0) {
			volume_index->sparse_sample_rate = header.sparse_sample_rate;
		} else if (volume_index->sparse_sample_rate != header.sparse_sample_rate) {
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
	if (result != UDS_SUCCESS)
		return result;
	return start_restoring_volume_sub_index(&volume_index->vi_hook,
						buffered_readers,
						num_readers);
}

static int finish_restoring_volume_sub_index(struct volume_sub_index *sub_index,
					     struct buffered_reader **buffered_readers,
					     unsigned int num_readers)
{
	return finish_restoring_delta_index(&sub_index->delta_index,
					    buffered_readers,
					    num_readers);
}

static int finish_restoring_volume_index(struct volume_index *volume_index,
					 struct buffered_reader **buffered_readers,
					 unsigned int num_readers)
{
	int result;

	result = finish_restoring_volume_sub_index(&volume_index->vi_non_hook,
						   buffered_readers,
						   num_readers);
	if ((result == UDS_SUCCESS) && has_sparse(volume_index))
		result = finish_restoring_volume_sub_index(&volume_index->vi_hook,
							   buffered_readers,
							   num_readers);
	return result;
}

int load_volume_index(struct volume_index *volume_index,
		      struct buffered_reader **readers,
		      unsigned int num_readers)
{
	int result;

	/* Start by reading the header section of the stream. */
	result = start_restoring_volume_index(volume_index, readers, num_readers);
	if (result != UDS_SUCCESS)
		return result;

	result = finish_restoring_volume_index(volume_index, readers, num_readers);
	if (result != UDS_SUCCESS) {
		abort_restoring_volume_index(volume_index);
		return result;
	}

	/* Check the final guard lists to make sure we read everything. */
	result = check_guard_delta_lists(readers, num_readers);
	if (result != UDS_SUCCESS)
		abort_restoring_volume_index(volume_index);

	return result;
}

static int __must_check
encode_volume_sub_index_header(struct buffer *buffer, struct sub_index_data *header)
{
	int result;

	result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START_5);
	if (result != UDS_SUCCESS)
		return result;
	result = put_u64_le_into_buffer(buffer, header->volume_nonce);
	if (result != UDS_SUCCESS)
		return result;
	result =
		put_u64_le_into_buffer(buffer, header->virtual_chapter_low);
	if (result != UDS_SUCCESS)
		return result;
	result = put_u64_le_into_buffer(buffer, header->virtual_chapter_high);
	if (result != UDS_SUCCESS)
		return result;
	result = put_u32_le_into_buffer(buffer, header->first_list);
	if (result != UDS_SUCCESS)
		return result;
	result = put_u32_le_into_buffer(buffer, header->num_lists);
	if (result != UDS_SUCCESS)
		return result;
	return ASSERT(content_length(buffer) == sizeof(struct sub_index_data),
		      "%zu bytes of config written, of %zu expected",
		      content_length(buffer),
		      sizeof(struct sub_index_data));
}

static int start_saving_volume_sub_index(const struct volume_sub_index *sub_index,
					 unsigned int zone_number,
					 struct buffered_writer *buffered_writer)
{
	int result;
	struct volume_sub_index_zone *volume_index_zone = &sub_index->zones[zone_number];
	unsigned int first_list = get_delta_zone_first_list(&sub_index->delta_index, zone_number);
	unsigned int num_lists = get_delta_zone_list_count(&sub_index->delta_index, zone_number);
	struct sub_index_data header;
	u64 *first_flush_chapter;
	struct buffer *buffer;

	memset(&header, 0, sizeof(header));
	memcpy(header.magic, MAGIC_START_5, MAGIC_SIZE);
	header.volume_nonce = sub_index->volume_nonce;
	header.virtual_chapter_low = volume_index_zone->virtual_chapter_low;
	header.virtual_chapter_high = volume_index_zone->virtual_chapter_high;
	header.first_list = first_list;
	header.num_lists = num_lists;

	result = make_buffer(sizeof(struct sub_index_data), &buffer);
	if (result != UDS_SUCCESS)
		return result;

	result = encode_volume_sub_index_header(buffer, &header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result, "failed to write volume index header");

	result = make_buffer(num_lists * sizeof(u64), &buffer);
	if (result != UDS_SUCCESS)
		return result;

	first_flush_chapter = &sub_index->flush_chapters[first_list];
	result = put_u64_les_into_buffer(buffer, num_lists, first_flush_chapter);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(buffered_writer,
					  get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS)
		return uds_log_warning_strerror(result,
						"failed to write volume index flush ranges");

	return start_saving_delta_index(&sub_index->delta_index, zone_number, buffered_writer);
}

static int __must_check
encode_volume_index_header(struct buffer *buffer, struct volume_index_data *header)
{
	int result;

	result = put_bytes(buffer, MAGIC_SIZE, MAGIC_START_6);
	if (result != UDS_SUCCESS)
		return result;
	result = put_u32_le_into_buffer(buffer, header->sparse_sample_rate);
	if (result != UDS_SUCCESS)
		return result;
	return ASSERT(content_length(buffer) == sizeof(struct volume_index_data),
		      "%zu bytes of config written, of %zu expected",
		      content_length(buffer),
		      sizeof(struct volume_index_data));
}

static int start_saving_volume_index(const struct volume_index *volume_index,
				     unsigned int zone_number,
				     struct buffered_writer *buffered_writer)
{
	struct volume_index_data header;
	struct buffer *buffer;
	int result;

	if (!has_sparse(volume_index))
		return start_saving_volume_sub_index(&volume_index->vi_non_hook,
						     zone_number,
						     buffered_writer);

	result = make_buffer(sizeof(struct volume_index_data), &buffer);
	if (result != UDS_SUCCESS)
		return result;
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
		uds_log_warning_strerror(result, "failed to write volume index header");
		return result;
	}

	result = start_saving_volume_sub_index(&volume_index->vi_non_hook,
					       zone_number,
					       buffered_writer);
	if (result != UDS_SUCCESS)
		return result;

	return start_saving_volume_sub_index(&volume_index->vi_hook, zone_number, buffered_writer);
}

static int
finish_saving_volume_sub_index(const struct volume_sub_index *sub_index, unsigned int zone_number)
{
	return finish_saving_delta_index(&sub_index->delta_index, zone_number);
}

static int
finish_saving_volume_index(const struct volume_index *volume_index, unsigned int zone_number)
{
	int result;

	result = finish_saving_volume_sub_index(&volume_index->vi_non_hook, zone_number);
	if ((result == UDS_SUCCESS) && has_sparse(volume_index))
		result = finish_saving_volume_sub_index(&volume_index->vi_hook, zone_number);
	return result;
}

int save_volume_index(struct volume_index *volume_index,
		      struct buffered_writer **writers,
		      unsigned int num_writers)
{
	int result = UDS_SUCCESS;
	unsigned int zone;

	for (zone = 0; zone < num_writers; ++zone) {
		result = start_saving_volume_index(volume_index, zone, writers[zone]);
		if (result != UDS_SUCCESS)
			break;

		result = finish_saving_volume_index(volume_index, zone);
		if (result != UDS_SUCCESS)
			break;

		result = write_guard_delta_list(writers[zone]);
		if (result != UDS_SUCCESS)
			break;

		result = flush_buffered_writer(writers[zone]);
		if (result != UDS_SUCCESS)
			break;
	}

	return result;
}

static void get_volume_sub_index_stats(const struct volume_sub_index *sub_index,
				       struct volume_index_stats *stats)
{
	struct delta_index_stats dis;
	unsigned int z;

	get_delta_index_stats(&sub_index->delta_index, &dis);
	stats->memory_allocated = (dis.memory_allocated + sizeof(struct volume_sub_index) +
				   sub_index->num_delta_lists * sizeof(u64) +
				   sub_index->num_zones * sizeof(struct volume_sub_index_zone));
	stats->rebalance_time = dis.rebalance_time;
	stats->rebalance_count = dis.rebalance_count;
	stats->record_count = dis.record_count;
	stats->collision_count = dis.collision_count;
	stats->discard_count = dis.discard_count;
	stats->overflow_count = dis.overflow_count;
	stats->num_lists = dis.list_count;
	stats->early_flushes = 0;
	for (z = 0; z < sub_index->num_zones; z++)
		stats->early_flushes += sub_index->zones[z].num_early_flushes;
}

void get_volume_index_stats(const struct volume_index *volume_index,
			    struct volume_index_stats *dense,
			    struct volume_index_stats *sparse)
{
	get_volume_sub_index_stats(&volume_index->vi_non_hook, dense);
	if (has_sparse(volume_index))
		get_volume_sub_index_stats(&volume_index->vi_hook, sparse);
	else
		memset(sparse, 0, sizeof(struct volume_index_stats));
}

#ifdef TEST_INTERNAL
void get_volume_index_combined_stats(const struct volume_index *volume_index,
				     struct volume_index_stats *stats)
{
	struct volume_index_stats dense, sparse;

	get_volume_index_stats(volume_index, &dense, &sparse);
	stats->memory_allocated = dense.memory_allocated + sparse.memory_allocated;
	stats->rebalance_time = dense.rebalance_time + sparse.rebalance_time;
	stats->rebalance_count = dense.rebalance_count + sparse.rebalance_count;
	stats->record_count = dense.record_count + sparse.record_count;
	stats->collision_count = dense.collision_count + sparse.collision_count;
	stats->discard_count = dense.discard_count + sparse.discard_count;
	stats->overflow_count = dense.overflow_count + sparse.overflow_count;
	stats->num_lists = dense.num_lists + sparse.num_lists;
	stats->early_flushes = dense.early_flushes + sparse.early_flushes;
}

#endif /* TEST_INTERNAL */
static int initialize_volume_sub_index(const struct configuration *config,
				       u64 volume_nonce,
				       struct volume_sub_index *sub_index)
{
	struct sub_index_parameters params = { .address_bits = 0 };
	unsigned int num_zones = config->zone_count;
	int result;

	result = compute_volume_index_parameters(config, &params);
	if (result != UDS_SUCCESS)
		return result;

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
	if (result != UDS_SUCCESS)
		return result;

	sub_index->max_zone_bits = ((get_delta_index_bits_allocated(&sub_index->delta_index) -
				     params.target_free_size * BITS_PER_BYTE) / num_zones);

	/* The following arrays are initialized to all zeros. */
	result = UDS_ALLOCATE(params.num_delta_lists,
			      u64,
			      "first chapter to flush",
			      &sub_index->flush_chapters);
	if (result != UDS_SUCCESS)
		return result;

	return UDS_ALLOCATE(num_zones,
			    struct volume_sub_index_zone,
			    "volume index zones",
			    &sub_index->zones);
}

int make_volume_index(const struct configuration *config,
		      u64 volume_nonce,
		      struct volume_index **volume_index_ptr)
{
	struct split_config split;
	unsigned int zone;
	struct volume_index *volume_index;
	int result;

	result = UDS_ALLOCATE(1, struct volume_index, "volume index", &volume_index);
	if (result != UDS_SUCCESS)
		return result;

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
	for (zone = 0; zone < config->zone_count; zone++)
		if (result == UDS_SUCCESS)
			result = uds_init_mutex(&volume_index->zones[zone].hook_mutex);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return result;
	}

	result = initialize_volume_sub_index(&split.non_hook_config,
					     volume_nonce,
					     &volume_index->vi_non_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return uds_log_error_strerror(result, "Error creating non hook volume index");
	}
	set_volume_index_tag(&volume_index->vi_non_hook, 'd');

	result = initialize_volume_sub_index(&split.hook_config,
					     volume_nonce,
					     &volume_index->vi_hook);
	if (result != UDS_SUCCESS) {
		free_volume_index(volume_index);
		return uds_log_error_strerror(result, "Error creating hook volume index");
	}
	set_volume_index_tag(&volume_index->vi_hook, 's');

	*volume_index_ptr = volume_index;
	return UDS_SUCCESS;
}
