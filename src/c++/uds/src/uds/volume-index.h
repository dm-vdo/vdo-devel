/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUMEINDEX_H
#define VOLUMEINDEX_H 1

#include "config.h"
#include "delta-index.h"
#include "uds.h"

#ifdef TEST_INTERNAL
extern unsigned int min_volume_index_delta_lists;

#endif /* TEST_INTERNAL */
struct volume_index_stats {
	size_t memory_allocated;    /* Number of bytes allocated */
	ktime_t rebalance_time;	    /* Nanoseconds spent rebalancing */
	int rebalance_count;        /* Number of memory rebalances */
	long record_count;          /* The number of records in the index */
	long collision_count;       /* The number of collision records */
	long discard_count;         /* The number of records removed */
	long overflow_count;        /* The number of UDS_OVERFLOWs detected */
	unsigned int num_lists;     /* The number of delta lists */
	long early_flushes;         /* Number of early flushes */
};

struct volume_index;
struct volume_sub_index;

/*
 * The volume_index_record structure is used for normal index read-write
 * processing of a record name.  The first call must be to
 * get_volume_index_record() to find the volume index record for a record name.
 * This call can be followed by put_volume_index_record() to add a volume
 * index record, or by set_volume_index_record_chapter() to associate the chunk
 * name with a different chapter, or by remove_volume_index_record() to delete
 * a volume index record.
 */
struct volume_index_record {
	/* Public fields */
	uint64_t virtual_chapter;  /* Chapter where the block info is found */
	bool is_collision;         /* This record is a collision */
	bool is_found;             /* This record is the block searched for */

	/* Private fields */
	unsigned char magic;                   /* The magic number for valid */
					       /* records */
	unsigned int zone_number;              /* Zone that contains this block */
	struct volume_sub_index *sub_index;    /* The volume index */
	struct mutex *mutex;                   /* Mutex that must be held while */
					       /* accessing this delta index */
					       /* entry; used only for a */
					       /* sampled index; otherwise is */
					       /* NULL */
	const struct uds_record_name *name;    /* The blockname to which */
					       /* this record refers */
	struct delta_index_entry delta_entry;  /* The delta index entry for */
					       /* this record */
};

/**
 * Make a new volume index.
 *
 * @param config        The configuration of the volume index
 * @param volume_nonce  The nonce used to store the index
 * @param volume_index  Location to hold new volume index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check make_volume_index(const struct configuration *config,
				   uint64_t volume_nonce,
				   struct volume_index **volume_index);

/**
 * Terminate and clean up the volume index
 *
 * @param volume_index The volume index to terminate
 **/
void free_volume_index(struct volume_index *volume_index);

/**
 * Compute the number of blocks required to save a volume index of a given
 * configuration.
 *
 * @param [in]  config           The configuration of a volume index
 * @param [in]  block_size       The size of a block in bytes.
 * @param [out] block_count      The resulting number of blocks.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
compute_volume_index_save_blocks(const struct configuration *config,
				 size_t block_size,
				 uint64_t *block_count);

#ifdef TEST_INTERNAL
/**
 * Get the number of bytes used for volume index entries.
 *
 * @param volume_index The volume index
 *
 * @return The number of bytes in use
 **/
size_t get_volume_index_memory_used(const struct volume_index *volume_index);

#endif /* TEST_INTERNAL */
/**
 * Find the volume index zone associated with a record name
 *
 * @param volume_index  The volume index
 * @param name          The record name
 *
 * @return the zone that the record name belongs to
 **/
unsigned int __must_check
get_volume_index_zone(const struct volume_index *volume_index,
		      const struct uds_record_name *name);

/**
 * Determine whether a given record name is a hook.
 *
 * @param volume_index  The volume index
 * @param name          The block name
 *
 * @return whether to use as sample
 **/
bool __must_check
is_volume_index_sample(const struct volume_index *volume_index,
		       const struct uds_record_name *name);

/**
 * Do a quick read-only lookup of the record name and return information
 * needed by the index code to process the record name.
 *
 * @param volume_index     The volume index
 * @param name             The record name
 *
 * @return The sparse virtual chapter, or UINT64_MAX if none
 **/
uint64_t __must_check
lookup_volume_index_name(const struct volume_index *volume_index,
			 const struct uds_record_name *name);

/**
 * Find the volume index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * volume index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If is_found is false, then we did not find an entry for the block name.
 * Information is saved in the volume_index_record so that
 * put_volume_index_record() will insert an entry for that block name at the
 * proper place.
 *
 * If is_found is true, then we did find an entry for the block name.
 * Information is saved in the volume_index_record so that the "chapter" and
 * "is_collision" fields reflect the entry found.  Calls to
 * remove_volume_index_record() will remove the entry, calls to
 * set_volume_index_record_chapter() can modify the entry, and calls to
 * put_volume_index_record() can insert a collision record with this entry.
 *
 * @param volume_index  The volume index to search
 * @param name          The record name
 * @param record        Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check get_volume_index_record(struct volume_index *volume_index,
					 const struct uds_record_name *name,
					 struct volume_index_record *record);

/**
 * Create a new record associated with a block name.
 *
 * @param record          The volume index record found by get_record()
 * @param virtual_chapter The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_volume_index_record(struct volume_index_record *record,
					 uint64_t virtual_chapter);

/**
 * Remove an existing record.
 *
 * @param record  The volume index record found by get_record()
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
remove_volume_index_record(struct volume_index_record *record);

/**
 * Set the open chapter number.  The volume index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * In normal operation, the virtual chapter number will be the next chapter
 * following the currently open chapter.  We will advance the volume index
 * one chapter forward in the virtual chapter space, invalidating the
 * oldest chapter in the index and be prepared to add index entries for the
 * newly opened chapter.
 *
 * In abnormal operation we make a potentially large change to the range of
 * chapters being indexed.  This happens when we are replaying chapters or
 * rebuilding an entire index.  If we move the open chapter forward, we
 * will invalidate many chapters (potentially the entire index).  If we
 * move the open chapter backward, we invalidate any entry in the newly
 * open chapter and any higher numbered chapter (potentially the entire
 * index).
 *
 * @param volume_index     The volume index
 * @param virtual_chapter  The new open chapter number
 **/
void set_volume_index_open_chapter(struct volume_index *volume_index,
				   uint64_t virtual_chapter);

/**
 * Set the chapter number associated with a block name.
 *
 * @param record           The volume index record found by get_record()
 * @param virtual_chapter  The chapter number where block info is now found.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
set_volume_index_record_chapter(struct volume_index_record *record,
				uint64_t virtual_chapter);

/**
 * Set the open chapter number on a zone.  The volume index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param volume_index     The volume index
 * @param zone_number      The zone number
 * @param virtual_chapter  The new open chapter number
 **/
void set_volume_index_zone_open_chapter(struct volume_index *volume_index,
					unsigned int zone_number,
					uint64_t virtual_chapter);

/**
 * Restore a volume index.
 *
 * @param volume_index  The volume index
 * @param readers       The readers to read from.
 * @param num_readers   The number of readers.
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check load_volume_index(struct volume_index *volume_index,
				   struct buffered_reader **readers,
				   unsigned int num_readers);

/**
 * Start restoring the volume index from multiple buffered readers
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check
start_restoring_volume_index(struct volume_index *volume_index,
			     struct buffered_reader **buffered_readers,
			     unsigned int num_readers);

/**
 * Finish restoring a volume index from an input stream.
 *
 * @param volume_index      The volume index to restore into
 * @param buffered_readers  The buffered readers to read the volume index from
 * @param num_readers       The number of buffered readers
 **/
int __must_check
finish_restoring_volume_index(struct volume_index *volume_index,
			      struct buffered_reader **buffered_readers,
			      unsigned int num_readers);

/**
 * Abort restoring a volume index from an input stream.
 *
 * @param volume_index  The volume index
 **/
void abort_restoring_volume_index(struct volume_index *volume_index);

int __must_check save_volume_index(struct volume_index *volume_index,
				   struct buffered_writer **writers,
				   unsigned int num_writers);

/**
 * Start saving a volume index to a buffered output stream.
 *
 * @param volume_index     The volume index
 * @param zone_number      The number of the zone to save
 * @param buffered_writer  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check
start_saving_volume_index(const struct volume_index *volume_index,
			  unsigned int zone_number,
			  struct buffered_writer *buffered_writer);

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
int __must_check
finish_saving_volume_index(const struct volume_index *volume_index,
			   unsigned int zone_number);

/**
 * Return the volume index stats.
 *
 * @param volume_index  The volume index
 * @param dense         Stats for the dense portion of the index
 * @param sparse        Stats for the sparse portion of the index
 **/
void get_volume_index_stats(const struct volume_index *volume_index,
			    struct volume_index_stats *dense,
			    struct volume_index_stats *sparse);

#ifdef TEST_INTERNAL
/**
 * Return the combined volume index stats.
 *
 * @param volume_index  The volume index
 * @param stats         Combined stats for the index
 **/
void get_volume_index_combined_stats(const struct volume_index *volume_index,
				     struct volume_index_stats *stats);

#endif /* TEST_INTERNAL */
#endif /* VOLUMEINDEX_H */
