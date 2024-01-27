// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * BiasedNames_n1 (formerly Index_x3 and BiasedNames_x1) indexes chunk
 * names that are not uniformly distributed and performs a full rebuild of
 * a chapter containing those names. Non-uniform distributions violate our
 * API contract and can cause very poor performance, but they should not
 * lead to a crash.
 *
 * Each "collisions" test zeros out a different range of the bytes in
 * 40,000 randomly-generated record names, ensuring that they are all either
 * volume index collisions, or chapter index collisions, etc.
 *
 * Each "copy" test copies a small random value multiple times to make
 * highly redundant record names, ensuring that each sub-field of the chunk
 * name shares the same randomness.
 **/

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "index.h"
#include "memory-alloc.h"
#include "testPrototypes.h"
#include "testRequests.h"

static struct block_device *testDevice;

/**********************************************************************/
static struct uds_index *createTestIndex(unsigned int loadFlags)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .bdev = testDevice,
  };
  struct uds_configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  struct uds_index *index;
  UDS_ASSERT_SUCCESS(uds_make_index(config, loadFlags, NULL, NULL, &index));
  uds_free_configuration(config);
  return index;
}

/**********************************************************************/
static void createMyMetadata(struct uds_record_data *data, const char *type)
{
  memset(data, 0, sizeof(*data));
  UDS_ASSERT_SUCCESS(uds_fixed_sprintf((char *) data->data, sizeof(*data),
                                       "%s", type));
}

/**********************************************************************/
static void insertChunk(struct uds_index             *index,
                        const struct uds_record_name *name,
                        const struct uds_record_data *data)
{
  struct uds_request request = {
    .record_name  = *name,
    .new_metadata = *data,
    .type         = UDS_UPDATE,
  };
  verify_test_request(index, &request, false, NULL);
}

/**********************************************************************/
static void updateChunk(struct uds_index             *index,
                        const struct uds_record_name *name,
                        const struct uds_record_data *oldData,
                        const struct uds_record_data *newData)
{
  struct uds_request request = {
    .record_name  = *name,
    .new_metadata = *newData,
    .type         = UDS_UPDATE,
    .zone_number  = uds_get_volume_index_zone(index->volume_index, name),
  };
  submit_test_request(index, &request);
  if (request.found) {
    UDS_ASSERT_BLOCKDATA_EQUAL(oldData, &request.old_metadata);
  }
}

/**********************************************************************/
static struct uds_index *rebuildIndex(struct uds_index *index)
{
  fillChapterRandomly(index);
  // Do a full rebuild from the volume file
  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  uds_free_index(index);
  return createTestIndex(UDS_LOAD);
}

/**********************************************************************/
static void doLotsaChunks(struct uds_index       *index,
                          int                     numChunks,
                          struct uds_record_name *names,
                          struct uds_record_data *oldData,
                          struct uds_record_data *newData)
{
  int i;
  for (i = 0; i < numChunks; i++) {
    if (oldData == NULL) {
      insertChunk(index, &names[i], newData);
    } else {
      updateChunk(index, &names[i], oldData, newData);
    }
  }
}

/**********************************************************************/
static void testWithNames(int numChunks, struct uds_record_name *names)
{
  struct uds_record_data data1, data2, data3;
  createMyMetadata(&data1, "First Data");
  createMyMetadata(&data2, "Second Data");
  createMyMetadata(&data3, "Third Data");

  struct uds_index *index = createTestIndex(UDS_CREATE);

  doLotsaChunks(index, numChunks, names, NULL, &data1);
  fillChapterRandomly(index);
  doLotsaChunks(index, numChunks, names, &data1, &data2);
  // Rebuild the index from the volume file
  index = rebuildIndex(index);
  doLotsaChunks(index, numChunks, names, &data2, &data3);
  uds_free_index(index);
}

/**********************************************************************/
static void testWithCollisions(int offset, int count)
{
  enum { NUM_CHUNKS = 40000 };
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(uds_allocate(NUM_CHUNKS, struct uds_record_name, "names",
                                  &names));
  int i;
  for (i = 0; i < NUM_CHUNKS; i++) {
    createRandomBlockName(&names[i]);
    memset(names[i].name + offset, 0, count);
  }
  testWithNames(NUM_CHUNKS, names);
  uds_free(names);
}

/**********************************************************************/
static void sampleTest(void)
{
  testWithCollisions(SAMPLE_BYTES_OFFSET, SAMPLE_BYTES_COUNT);
}

/**********************************************************************/
static void chapterIndexTest(void)
{
  testWithCollisions(CHAPTER_INDEX_BYTES_OFFSET, CHAPTER_INDEX_BYTES_COUNT);
}

/**********************************************************************/
static void volumeIndexTest(void)
{
  testWithCollisions(VOLUME_INDEX_BYTES_OFFSET, VOLUME_INDEX_BYTES_COUNT);
}

/**********************************************************************/
static void copy32Test(void)
{
  enum { NUM_CHUNKS = 40000 };
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(uds_allocate(NUM_CHUNKS, struct uds_record_name, "names",
                                  &names));
  int i, j, k;
  for (i = 0; i < NUM_CHUNKS; i++) {
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
  testWithNames(NUM_CHUNKS, names);
  uds_free(names);
}

/**********************************************************************/
static void initializerWithBlockDevice(struct block_device *bdev)
{
  testDevice = bdev;
  initialize_test_requests();
}

/**********************************************************************/
static void deinit(void)
{
  uninitialize_test_requests();
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Sample Collisions Test",       sampleTest },
  { "Chapter Collisions Test",      chapterIndexTest },
  { "Volume Index Collisions Test", volumeIndexTest },
  { "32 Bit Test",                  copy32Test },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "BiasedNames_n1",
  .initializerWithBlockDevice = initializerWithBlockDevice,
  .cleaner                    = deinit,
  .tests                      = tests,
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
