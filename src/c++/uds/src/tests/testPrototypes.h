/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef TEST_PROTOTYPES_H
#define TEST_PROTOTYPES_H

#include <linux/random.h>

#include "config.h"
#include "delta-index.h"
#include "hash-utils.h"
#include "index.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "murmurhash3.h"
#include "numeric.h"
#include "oldInterfaces.h"
#include "time-utils.h"
#include "uds.h"

enum {
	KILOBYTE = 1024,
	MEGABYTE = KILOBYTE * KILOBYTE,
	GIGABYTE = KILOBYTE * MEGABYTE
};

/**
 * Callback that gets invoked by UDS library on to report an asynchronous
 * result in block mode.  It asserts that the status returned UDS_SUCCESS.
 **/
void cbStatus(enum uds_request_type   type,
              int                     status,
              OldCookie               cookie,
              struct uds_record_data *duplicateAddress,
              struct uds_record_data *canonicalAddress,
              struct uds_record_name *blockName,
              void                   *data);

/**
 * Copy the first n bytes of one device to another device.
 *
 * @param source       The device to copy from.
 * @param destination  The device to copy into.
 * @param bytes        The number of bytes to copy.
 *
 * @return UDS_SUCCESS or an error code if the copy fails
 **/
int copyDevice(struct block_device *source,
	       struct block_device *destination,
	       off_t bytes);

/**
 * Create a block name that collides with the given block in the volume index.
 *
 * @param [in] orig             the original block name
 * @param [out] collision       the colliding block name
 **/
void createCollidingBlock(const struct uds_record_name *orig,
                          struct uds_record_name       *collision);

/**
 * Create a configuration for an albtest.  Takes as arguments the argc and
 * argv that are passed to the initSuite method of the test.
 *
 * @param argc  Command line arguments left over from albtest
 * @param argv  Command line arguments left over from albtest
 *
 * @return The new index configuration
 **/
struct configuration *createConfigForAlbtest(int argc, const char **argv)
  __attribute__((warn_unused_result));

/**
 * Create a random block name.
 *
 * @param [out] name    the resulting random block name
 **/

static inline void createRandomBlockName(struct uds_record_name *name)
{
  get_random_bytes(name->name, UDS_RECORD_NAME_SIZE);
}

/**
 * Create a random block name in specific zone.  Due to rounding, this
 * is only guaranteed to work for the highest numbered zone.
 *
 * @param [in]  index  the index containing the zones
 * @param [in]  zone   the target zone number
 * @param [out] name   the resulting random block name
 **/
void createRandomBlockNameInZone(const struct uds_index *index,
                                 unsigned int            zone,
                                 struct uds_record_name *name);

/**
 * Create random block metadata.
 *
 * @param [out] data    the result random metadata
 **/
static inline void createRandomMetadata(struct uds_record_data *data)
{
  get_random_bytes(data->data, UDS_RECORD_DATA_SIZE);
}

/**
 * Create a set of parameters for an albtest.  Takes as arguments the argc and
 * argv that are passed to the initSuite method of the test.
 *
 * @param argc  Command line arguments left over from albtest
 * @param argv  Command line arguments left over from albtest
 *
 * @return The UDS parameters
 **/
struct uds_parameters createUdsParametersForAlbtest(int          argc,
                                                    const char **argv)
  __attribute__((warn_unused_result));

/**
 * Fill a buffer with data patterned on a seed.
 *
 * @param seed          A seed value.
 * @param buffer        The buffer to fill.
 * @param size          The size of the buffer to fill.
 *
 * This function always succeeds, using MurmurHash to generate buffer
 * data.
 *
 * @return a potential new seed generated from the old seed
 **/
uint64_t fillBufferFromSeed(uint64_t seed, void *buffer, size_t size);

/**
 * Fill the open chapter with random blocks until it is closed.
 *
 * @param index  the index to use
 **/
void fillChapterRandomly(struct uds_index *index);

/**
 * Get the total memory of the system in gigabytes.  The intent of this
 * method is to determine the largest size index we should be testing.
 *
 * @return physical memory size in megabytes.
 **/
size_t getMemTotalInGB(void) __attribute__((warn_unused_result));

/**********************************************************************/
static inline void freeRequest(struct uds_request *request)
{
  if (request != NULL) {
    UDS_FREE(request);
  }
}

#ifndef __KERNEL__
/**
 * Get test index names.  The index names are platform-specific, and
 * therefore this method is defined by platform dependent code.
 *
 * @return a pointer to a null-terminated list of char* pointers.  Each
 *         entry in the list is a string naming an index and suitable for
 *         passing to udsCreateLocalIndex or udsLoadLocalIndex.  The first
 *         entry is the primary index name and will be used by any test
 *         that wants to operate on only one index.
 **/
const char *const *getTestIndexNames(void) __attribute__((warn_unused_result));

/**
 * Get the primary test index name, which is the first name in the list
 * returned from getTestIndexNames.
 *
 * @return The primary test index name
 **/
__attribute__((warn_unused_result))
static inline const char *getTestIndexName(void)
{
  return *getTestIndexNames();
}

/**
 * Get test index names for indices that can be used at the same time in a
 * multiindex test.  The index names are platform-specific, and therefore
 * this method is defined by platform dependent code.
 *
 * @return an array of names that can each be passed to uds_open_index()
 **/
const char *const *getTestMultiIndexNames(void)
  __attribute__((warn_unused_result));

#endif /* __KERNEL__ */
/**
 * Get the primary test block device, which is the created from the name
 * returned by getTestIndexName().
 *
 * @return The primary test block device
 **/
struct block_device *getTestBlockDevice(void)
  __attribute__((warn_unused_result));

/**
 * Get test index block devices for indices that can be used at the same time
 * in a multi-index test.
 *
 * @return an array of devices that can each be passed to uds_open_index()
 **/
struct block_device **getTestMultiBlockDevices(void)
  __attribute__((warn_unused_result));

/**
 * Close a test block device. Must be called after getTestBlockDevice() or
 * getTestMultiBlockDevices().
 *
 * @param device  The test block device
 **/
void putTestBlockDevice(struct block_device *device);

/**
 * Make a test configuration for a dense index
 *
 * @param memGB  The maximum memory allocation, in GB
 *
 * @return the configuration
 **/
struct configuration *makeDenseConfiguration(uds_memory_config_size_t memGB)
  __attribute__((warn_unused_result));

/**
 * Quickly generate a non-cryptographic hash of a chunk of data using the
 * 128-bit MurmurHash3 algorithm with the seed that VDO uses.
 *
 * @param [in] data  A pointer to the opaque data
 * @param [in] size  The size of the data, in bytes
 *
 * @return The calculated record name
 **/
static inline struct uds_record_name hash_record_name(const void *data,
                                                      size_t      size)
{
  enum { SEED1 = 0x62ea60be };
  struct uds_record_name name;
  murmurhash3_128(data, size, SEED1, &name.name[0]);
  return name;
}

/**
 * Set the nonce in the uds_parameters to a randomly chosen value.
 *
 * @param params  The index parameters
 **/
static inline void randomizeUdsNonce(struct uds_parameters *params)
{
  get_random_bytes(&params->nonce, sizeof(params->nonce));
}

/*
 * Format the supplied time as a string. Upon a success, the caller must
 * UDS_FREE the returned string.
 */
int __must_check rel_time_to_string(char **strp, ktime_t reltime);

/**
 * Recompute a configuration with different parameters. Any parameter set to
 * zero will be unchanged from the previous value.
 *
 * @param config                    The configuration to change
 * @param bytes_per_page            The new number of bytes per page
 * @param record_pages_per_chapter  The new number of record pages
 * @param chapters_per_volume       The new number of chapters per volume
 **/
void resizeDenseConfiguration(struct configuration *config,
                              size_t bytes_per_page,
                              unsigned int record_pages_per_chapter,
                              unsigned int chapters_per_volume);

/**
 * Recompute a configuration with different parameters. Any parameter set to
 * zero will be unchanged from the previous value.
 *
 * @param config                      The configuration to change
 * @param bytes_per_page              The new number of bytes per page
 * @param record_pages_per_chapter    The new number of record pages
 * @param chapters_per_volume         The new number of chapters per volume
 * @param sparse_chapters_per_volume  The new number of sparse chapters
 * @param sparse_sample_rate          The new sparse sample rate
 **/
void resizeSparseConfiguration(struct configuration *config,
                               size_t bytes_per_page,
                               unsigned int record_pages_per_chapter,
                               unsigned int chapters_per_volume,
                               unsigned int sparse_chapters_per_volume,
                               unsigned int sparse_sample_rate);

/**
 * Set the portion of a block name used by the chapter index.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static inline void set_chapter_index_bytes(struct uds_record_name *name,
                                           uint64_t                value)
{
        /* Store the high order bytes, then the low-order bytes */
        put_unaligned_be16((uint16_t)(value >> 32),
                           &name->name[CHAPTER_INDEX_BYTES_OFFSET]);
        put_unaligned_be32((uint32_t) value,
                           &name->name[CHAPTER_INDEX_BYTES_OFFSET + 2]);
}

/**
 * Set the bits used to find a chapter delta list
 *
 * @param name     The block name
 * @param geometry The geometry to use
 * @param value    The value to store
 **/
static inline void set_chapter_delta_list_bits(struct uds_record_name *name,
                                               const struct geometry *geometry,
                                               uint64_t value)
{
        uint64_t delta_address = uds_hash_to_chapter_delta_address(name, geometry);

        delta_address |= value << geometry->chapter_address_bits;
        set_chapter_index_bytes(name, delta_address);
}

/**
 * Set the portion of a block name used for sparse sampling.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static inline void set_sampling_bytes(struct uds_record_name *name,
                                      uint32_t                value)
{
        put_unaligned_be16((uint16_t) value, &name->name[SAMPLE_BYTES_OFFSET]);
}

/**
 * Set the portion of a block name used by the volume index.
 *
 * @param name  The block name
 * @param val   The value to store
 **/
static inline void set_volume_index_bytes(struct uds_record_name *name,
                                          uint64_t                val)
{
        put_unaligned_be64(val, &name->name[VOLUME_INDEX_BYTES_OFFSET]);
}

void sleep_for(ktime_t reltime);

static inline ktime_t seconds_to_ktime(int64_t seconds)
{
        return (ktime_t) seconds * NSEC_PER_SEC;
}

static inline ktime_t us_to_ktime(int64_t microseconds)
{
        return (ktime_t) microseconds * NSEC_PER_USEC;
}

/**
 * Validate the delta list headers.
 *
 * @param delta_zone  A delta memory structure
 **/
void validateDeltaLists(const struct delta_zone *delta_zone);

#endif /* TEST_PROTOTYPES_H */
