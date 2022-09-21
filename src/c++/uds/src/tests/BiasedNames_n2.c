// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * BiasedNames_n2 indexes record names that are not uniformly distributed
 * using the UDS interfaces.  Non-uniform distributions violate our API
 * contract and can cause very poor performance, but they should not lead
 * to a crash.
 *
 * This test differs from BiasedNames_n1 in that it uses the UDS interfaces on
 * all types of indices (dense/sparse and local/remote).  Thus doing things to
 * the sample field actually invokes the effect of the field.  On the other
 * hand, BiasedNames_n1 uses the Index interfaces and tests the index
 * rebuilding code paths.
 *
 * Each "collisions" test sets a different range of the bytes in 40,000
 * randomly-generated record names, ensuring that they are all either volume
 * index collisions, or chapter index collisions, or sub-index collisions,
 * etc.
 *
 * Each "copy" test copies a small random value multiple times to make
 * highly redundant record names, ensuring that each sub-field of the chunk
 * name shares the same randomness.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "hash-utils.h"
#include "index.h"
#include "index-session.h"
#include "oldInterfaces.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

static struct uds_index_session *indexSession;

/**********************************************************************/
static void cb(enum uds_request_type type __attribute__((unused)),
               int status,
               OldCookie cookie,
               struct uds_chunk_data *duplicateAddress __attribute__((unused)),
               struct uds_chunk_data *canonicalAddress,
               struct uds_record_name *blockName __attribute__((unused)),
               void *data __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(status);
  if (canonicalAddress != NULL) {
    CU_ASSERT_PTR_NOT_NULL(cookie);
    UDS_ASSERT_EQUAL_BYTES(cookie, canonicalAddress,
                           sizeof(struct uds_chunk_data));
  }
}

/**********************************************************************/
__printf(2, 3)
static void createMyMetadata(struct uds_chunk_data *data, const char *fmt, ...)
{
  int bytes;
  va_list args;

  va_start(args, fmt);
  memset(data, 0, sizeof(*data));
  bytes = vsnprintf((char *) data->data, sizeof(*data), fmt, args);
  va_end(args);
  CU_ASSERT_TRUE((bytes >= 0) && ((size_t) bytes < sizeof(*data)));
}

/**********************************************************************/
static void createCopy32Names(int numChunks, struct uds_record_name *names)
{
  int i, j, k;
  for (i = 0; i < numChunks; i++) {
  try_again:
    createRandomBlockName(&names[i]);
    for (j = 0; j < i; j++) {
      if (memcmp(names[i].name, names[j].name, 4) == 0) {
        goto try_again;
      }
    }
    for (k = 4; k < UDS_RECORD_NAME_SIZE; k += 4) {
      memcpy(&names[i].name[k], &names[i].name[0], 4);
    }
  }
}

/**********************************************************************/
static void createCollisionNames(int                     numChunks,
                                 struct uds_record_name *names,
                                 int                     offset,
                                 int                     count,
                                 byte                    filler)
{
  int i;
  for (i = 0; i < numChunks; i++) {
    createRandomBlockName(&names[i]);
    memset(names[i].name + offset, filler, count);
  }
}

/**********************************************************************/
static void testWithChunks(struct uds_index_session *indexSession,
                           int                       numChunks,
                           struct uds_record_name   *names,
                           const char               *type)
{
  struct uds_chunk_data data1, data2, dataFill;
  createMyMetadata(&data1, "1st %s", type);
  createMyMetadata(&data2, "2nd %s", type);
  createMyMetadata(&dataFill, "Fill %s", type);
  // Insert the chunks into the index
  int i;
  for (i = 0; i < numChunks; i++) {
    oldPostBlockName(indexSession, NULL, &data1, &names[i], cb);
  }
  // Age the chunks in the index
  int ageCount = 2 * getBlocksPerChapter(indexSession);
  for (i = 0; i < ageCount; i++) {
    struct uds_record_name name;
    createRandomBlockName(&name);
    oldPostBlockName(indexSession, NULL, &dataFill, &name, cb);
  }
  // Update the chunks in the index
  for (i = 0; i < numChunks; i++) {
    oldUpdateBlockMapping(indexSession, &data1, &names[i], &data2, cb);
  }
  // Need to wait for all updates to complete because the callback will be
  // accessing data1
  UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));
}

/**********************************************************************/
static void runTest(void)
{
  enum { NUM_CHUNKS = 40000 };
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_CHUNKS, struct uds_record_name, "names",
                                  &names));
  initializeOldInterfaces(2000);

  // Sample field: Hooks testing
  createCollisionNames(NUM_CHUNKS, names, SAMPLE_BYTES_OFFSET,
                       SAMPLE_BYTES_COUNT, 0);
  testWithChunks(indexSession, NUM_CHUNKS, names, "Hooks");

  // Sample field: Non-Hooks testing
  createCollisionNames(NUM_CHUNKS, names, SAMPLE_BYTES_OFFSET,
                       SAMPLE_BYTES_COUNT, ~0);
  testWithChunks(indexSession, NUM_CHUNKS, names, "NonHooks");

  // Volume field
  createCollisionNames(NUM_CHUNKS, names, CHAPTER_INDEX_BYTES_OFFSET,
                       CHAPTER_INDEX_BYTES_COUNT, 0);
  testWithChunks(indexSession, NUM_CHUNKS, names, "Chapter");

  // Volume Index field
  createCollisionNames(NUM_CHUNKS, names, VOLUME_INDEX_BYTES_OFFSET,
                       VOLUME_INDEX_BYTES_COUNT, 0);
  testWithChunks(indexSession, NUM_CHUNKS, names, "Volume");

  // Copies of the same 32 bits
  createCopy32Names(NUM_CHUNKS, names);
  testWithChunks(indexSession, NUM_CHUNKS, names, "Copy Bits");

  UDS_FREE(names);
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithSession(struct uds_index_session *is)
{
  indexSession = is;

  struct uds_parameters *params;
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(indexSession, &params));
  if (params->sparse) {
    struct configuration *config;
    UDS_ASSERT_SUCCESS(make_configuration(params, &config));
    unsigned int chapters_per_volume = config->geometry->chapters_per_volume;
    resizeSparseConfiguration(config, 0, 0, 0, chapters_per_volume - 2, 0);

    // Remake the index with the modified configuration.
    struct uds_index *oldIndex = indexSession->index;
    struct uds_index *newIndex;
    UDS_ASSERT_SUCCESS(make_index(config, UDS_CREATE, oldIndex->load_context,
                                  oldIndex->callback, &newIndex));
    indexSession->index = newIndex;
    free_index(oldIndex);
    free_configuration(config);
  }
  UDS_FREE(params);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "Biased Names", runTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                   = "BiasedNames_n2",
  .initializerWithSession = initializerWithSession,
  .tests                  = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
