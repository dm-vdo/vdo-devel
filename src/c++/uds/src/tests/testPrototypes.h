/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef TEST_PROTOTYPES_H
#define TEST_PROTOTYPES_H

#include <linux/murmurhash3.h>
#include <linux/prandom.h>

#include "compiler.h"
#include "config.h"
#include "delta-index.h"
#include "hash-utils.h"
#include "index.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "oldInterfaces.h"
#include "type-defs.h"
#include "uds.h"

/**
 * Callback that gets invoked by UDS library on to report an asynchronous
 * result in block mode.  It asserts that the status returned UDS_SUCCESS.
 **/
void cbStatus(enum uds_request_type type,
              int status,
              OldCookie cookie,
              struct uds_chunk_data *duplicateAddress,
              struct uds_chunk_data *canonicalAddress,
              struct uds_chunk_name *blockName,
              void *data);

/**
 * Copy the first n bytes of one device to another device.
 *
 * @param source       The device to copy from.
 * @param destination  The device to copy into.
 * @param bytes        The number of bytes to copy.
 *
 * @return UDS_SUCCESS or an error code if the copy fails
 **/
int copyDevice(const char *source, const char *destination, off_t bytes);

/**
 * Create a block name that collides with the given block in the volume index.
 *
 * @param [in] orig             the original block name
 * @param [out] collision       the colliding block name
 **/
void createCollidingBlock(const struct uds_chunk_name *orig,
                          struct uds_chunk_name *collision);

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

static INLINE void createRandomBlockName(struct uds_chunk_name *name)
{
  prandom_bytes(name->name, UDS_CHUNK_NAME_SIZE);
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
                                 struct uds_chunk_name  *name);

/**
 * Create random block metadata.
 *
 * @param [out] data    the result random metadata
 **/
static INLINE void createRandomMetadata(struct uds_chunk_data *data)
{
  prandom_bytes(data->data, UDS_METADATA_SIZE);
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
static INLINE void freeRequest(struct uds_request *request)
{
  if (request != NULL) {
    UDS_FREE(request);
  }
}

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
static INLINE const char *getTestIndexName(void)
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
 * 128-bit MurmurHash3 algorithm.
 *
 * @param [in] data  A pointer to the opaque data
 * @param [in] size  The size of the data, in bytes
 * @param [in] seed  A seed value for the hash calculation
 *
 * @return The calculated chunk name
 **/
static INLINE struct uds_chunk_name murmurHashChunkName(const void *data,
                                                        size_t      size,
                                                        uint32_t    seed)
{
  // A pair of randomly-generated seed values for the two hash computations.
  enum { SEED1 = 0x62ea60be };
  struct uds_chunk_name name;
  murmurhash3_128(data, size, SEED1 ^ seed, &name.name[0]);
  return name;
}

/**
 * Quickly generate a non-cryptographic hash of a chunk of data using the
 * 128-bit MurmurHash3 algorithm with a default seed.
 *
 * @param [in] data  A pointer to the opaque data
 * @param [in] size  The size of the data, in bytes
 *
 * @return The calculated chunk name
 **/
static INLINE struct uds_chunk_name murmurGenerator(const void *data,
                                                    size_t size)
{
  return murmurHashChunkName(data, size, 0);
}

/**
 * Set the nonce in the uds_parameters to a randomly chosen value.
 *
 * @param params  The index parameters
 **/
static INLINE void randomizeUdsNonce(struct uds_parameters *params)
{
  prandom_bytes(&params->nonce, sizeof(params->nonce));
}

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
static INLINE void set_chapter_index_bytes(struct uds_chunk_name *name,
					   uint64_t value)
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
static INLINE void set_chapter_delta_list_bits(struct uds_chunk_name *name,
					       const struct geometry *geometry,
					       uint64_t value)
{
	uint64_t delta_address = hash_to_chapter_delta_address(name, geometry);

	delta_address |= value << geometry->chapter_address_bits;
	set_chapter_index_bytes(name, delta_address);
}

/**
 * Set the portion of a block name used for sparse sampling.
 *
 * @param name   The block name
 * @param value  The value to store
 **/
static INLINE void set_sampling_bytes(struct uds_chunk_name *name,
				      uint32_t value)
{
	put_unaligned_be16((uint16_t) value, &name->name[SAMPLE_BYTES_OFFSET]);
}

/**
 * Set the portion of a block name used by the volume index.
 *
 * @param name  The block name
 * @param val   The value to store
 **/
static INLINE void set_volume_index_bytes(struct uds_chunk_name *name,
					  uint64_t val)
{
	put_unaligned_be64(val, &name->name[VOLUME_INDEX_BYTES_OFFSET]);
}

/**
 * Validate the delta list headers.
 *
 * @param delta_zone  A delta memory structure
 **/
void validateDeltaLists(const struct delta_zone *delta_zone);

#endif /* TEST_PROTOTYPES_H */
