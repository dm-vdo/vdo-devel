// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * FullRebuild_x1 (formerly Index_x2) deterministically fills dense and sparse
 * indexes, performs a full rebuild, then verifies that every indexed name is
 * still present in the index after the rebuild (sparse non-hook names are not
 * verified).
 *
 * Sparse and dense indexes are verified in tiny configurations (only eight
 * chapters) in which every possible case for the open physical chapter is
 * tested. Those tiny tests are too slow even for unit tests, at a few minutes
 * each. A default dense index is also tested, and that case takes well over
 * an hour to complete (likely due the lack of any concurrency).
 **/

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "hash-utils.h"
#include "index.h"
#include "testPrototypes.h"
#include "testRequests.h"

static struct block_device *testDevice;
static uint64_t             nameCounter = 0;

/**
 * The suite initialization function.
 **/
static void initSuite(struct block_device *bdev)
{
  testDevice = bdev;
  initialize_test_requests();
}

/**
 * The suite cleanup function.
 **/
static void cleanSuite(void)
{
  uninitialize_test_requests();
}

/**********************************************************************/
static uint64_t fillIndex(struct uds_index *index, unsigned long numRecords)
{
  uint64_t nameSeed = nameCounter;
  struct uds_request request = { .type = UDS_UPDATE };
  unsigned long i;
  for (i = 0; i < numRecords; i++) {
    request.record_name = hash_record_name(&nameCounter, sizeof(nameCounter));
    nameCounter++;
    verify_test_request(index, &request, false, NULL);
  }
  return nameSeed;
}

/**********************************************************************/
static void verifyData(struct uds_index *index,
                       unsigned long     numRecords,
                       uint64_t          nameSeed,
                       bool              sparse)
{
  struct uds_request request = { .type = UDS_QUERY_NO_UPDATE };
  unsigned long i;
  for (i = 0; i < numRecords; i++) {
    request.record_name = hash_record_name(&nameSeed, sizeof(nameSeed));
    nameSeed++;

    if (sparse) {
      // just verify the hooks for simplicity
      bool hook = uds_is_volume_index_sample(index->volume_index, &request.record_name);
      if (!hook) {
        continue;
      }
    }
    verify_test_request(index, &request, true, NULL);
  }
}

/**********************************************************************/
static void runTest(struct configuration *config, unsigned int prefillChapters)
{
  config->zone_count = 1;
  config->bdev = testDevice;

  struct uds_index *index;
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_CREATE, NULL, NULL, &index));
  struct index_geometry *geometry = config->geometry;
  unsigned long recordsPerChapter = geometry->records_per_chapter;
  unsigned long numSparseRecords
    = recordsPerChapter * geometry->sparse_chapters_per_volume;
  unsigned long numDenseRecords
    = recordsPerChapter * (geometry->chapters_per_volume - 1)
        - numSparseRecords;

  // Prefill some chapters of the index.
  // These data will be LRUed away when we are done filling.
  fillIndex(index, prefillChapters * recordsPerChapter);

  // Fill the sparse chapters of the index.
  // These data will be in the sparse index when we are done filling.
  unsigned long numRecords1 = numSparseRecords ? recordsPerChapter : 0;
  uint64_t seed1 = fillIndex(index, numRecords1);
  unsigned long numRecords2 = numSparseRecords - numRecords1;
  uint64_t seed2 = fillIndex(index, numRecords2);

  // Fill all but one of the dense chapters of the index.
  // These data will be in the dense index when we are done filling.
  unsigned long numRecords3 = recordsPerChapter;
  uint64_t seed3 = fillIndex(index, numRecords3);
  unsigned long numRecords4 = numDenseRecords - numRecords3;
  uint64_t seed4 = fillIndex(index, numRecords4);

  // Rebuild the volume index.
  UDS_ASSERT_SUCCESS(uds_save_index(index));
  UDS_ASSERT_SUCCESS(discard_index_state_data(index->layout));
  uds_free_index(index);
  UDS_ASSERT_SUCCESS(uds_make_index(config, UDS_LOAD, NULL, NULL, &index));

  // Verify the filled data.
  verifyData(index, numRecords1, seed1, true);
  verifyData(index, numRecords2, seed2, true);
  verifyData(index, numRecords3, seed3, false);
  verifyData(index, numRecords4, seed4, false);

  // Add one more chapter to the index.
  unsigned long numRecords5 = recordsPerChapter;
  uint64_t seed5 = fillIndex(index, numRecords5);

  // Verify the modified data.
  verifyData(index, numRecords2, seed2, true);
  verifyData(index, numRecords3, seed3, true);
  verifyData(index, numRecords4, seed4, false);
  verifyData(index, numRecords5, seed5, false);

  uds_free_index(index);
}

/**********************************************************************/
static void runTestsAtAllChapterOffsets(struct configuration *config)
{
  unsigned int i;
  for (i = 0; i < config->geometry->chapters_per_volume; i++) {
    runTest(config, i);
  }
}

/**********************************************************************/
static void testDenseTiny(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, 0, 0, 8);

  runTestsAtAllChapterOffsets(config);
  uds_free_configuration(config);
}

/**********************************************************************/
static void testSparseTiny(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .sparse = true,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeSparseConfiguration(config, 0, 0, 8, 4, 2);

  runTestsAtAllChapterOffsets(config);
  uds_free_configuration(config);
}

/**********************************************************************/
static void testDenseNormal(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));

  runTest(config, 0);
  uds_free_configuration(config);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Dense Tiny",   testDenseTiny   },
  {"Sparse Tiny",  testSparseTiny  },
  {"Dense Normal", testDenseNormal },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "FullRebuild_x1",
  .initializerWithBlockDevice = initSuite,
  .cleaner                    = cleanSuite,
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
