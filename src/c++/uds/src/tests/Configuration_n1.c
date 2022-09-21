// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

/**
 * Test UDS configuration
 *
 * We run a testcase for each supported memory size up to 2GB, and one
 * additional testcase that uses up to 8GB of the memory of the host machine.
 *
 * For each memory size we make sure the configuration and geometry has the
 * expected values.  If a change is made to the default settings, it is
 * necessary to also change this test.
 *
 * For each memory size, Albireo computes the number of pages that will be
 * used for a chapter index.  We fill 16 chapters of the index and require
 * that we drop no entries due to a page overflow or list overflow`.
 *
 * For each memory size we make sure that the memory usage is less than 102% of
 * the target memory size.
 **/

#include "albtest.h"
#include "assertions.h"
#include "chapter-index.h"
#include "config.h"
#include "hash-utils.h"
#include "logger.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "testRequests.h"

static const char *indexName;

/**********************************************************************/
typedef struct testConfig {
  // --mem and sparse options for the test
  uds_memory_config_size_t memGB;
  bool sparse;
  // Expected configuration values
  unsigned int recordPagesPerChapter;
  unsigned int chaptersPerVolume;
  // Expected Geometry values
  unsigned int indexPagesPerChapter;
  // Expected resource usage
  size_t memoryUsed;
} TestConfig;

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  indexName = name;
}

/**********************************************************************/
static void savedTest(void)
{
  initializeOldInterfaces(1000);
  initialize_test_requests();

  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
  };
  randomizeUdsNonce(&params);

  struct configuration *config;
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));

  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));

  // Fill 16 chapters
  uint64_t counter;
  uint64_t recordCount = 16 * config->geometry->records_per_chapter;
  for (counter = 0; counter < recordCount; counter++) {
    struct uds_record_name chunkName = murmurHashChunkName(&counter,
							   sizeof(counter),
							   0);
    oldPostBlockName(indexSession, NULL,
                     (struct uds_chunk_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  
  // Test that the index saved the configuration we created it with
  struct uds_parameters *savedParams;
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params, indexSession));
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(indexSession, &savedParams));
  CU_ASSERT_STRING_EQUAL(params.name, savedParams->name);
  UDS_ASSERT_EQUAL_BYTES(&params.size, &savedParams->size,
                         sizeof(params) - sizeof(params.name));

  // Test that the saved configuration can be used to reopen the index.
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, savedParams,
                                    indexSession));
  UDS_FREE(savedParams);

  // Verify data
  struct uds_request request = { .type = UDS_QUERY_NO_UPDATE };
  for (counter = 0; counter < recordCount; counter++) {
    request.record_name = murmurHashChunkName(&counter, sizeof(counter), 0);
    counter++;
    verify_test_request(indexSession->index, &request, true, NULL);
  }
  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

  // Test that the saved configuration persists after the index is closed.
  UDS_ASSERT_SUCCESS(uds_get_index_parameters(indexSession, &savedParams));
  CU_ASSERT_STRING_EQUAL(params.name, savedParams->name);
  UDS_ASSERT_EQUAL_BYTES(&params.size, &savedParams->size,
                         sizeof(params) - sizeof(params.name));
  UDS_FREE(savedParams);

  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  free_configuration(config);

  uninitialize_test_requests();
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void testRun(TestConfig *tc)
{
  // Test that the user configuration is as expected
  struct uds_parameters params = {
    .memory_size = tc->memGB,
    .name = indexName,
    .sparse = tc->sparse,
  };
  randomizeUdsNonce(&params);

  // Test that the geometry is as expected
  struct configuration *config;
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  CU_ASSERT_EQUAL(DEFAULT_BYTES_PER_PAGE, config->geometry->bytes_per_page);
  CU_ASSERT_EQUAL(tc->recordPagesPerChapter,
                  config->geometry->record_pages_per_chapter);
  CU_ASSERT_EQUAL(tc->chaptersPerVolume,
                  config->geometry->chapters_per_volume);
  CU_ASSERT_EQUAL(tc->recordPagesPerChapter,
                  config->geometry->record_pages_per_chapter);
  CU_ASSERT_EQUAL(tc->chaptersPerVolume,
                  config->geometry->chapters_per_volume);
  CU_ASSERT_EQUAL(tc->indexPagesPerChapter,
                  config->geometry->index_pages_per_chapter);
  /*
   * Test that we can create an index.  There are three possible failures that
   * are acceptable to this test.
   *
   * If the system does not have enough physical memory to open the index, we
   * can get an -ENOMEM error.
   *
   * If the storage device does not have enough space to store the index, we
   * can get an -ENOSPC error.
   *
   * If the filesystem does not support a file of the desired size, we can get
   * an -EFBIG error.  We have seen an ext3 filesystem give us an EFBIG error
   * when we try to create a 2.5TB file.
   *
   * An -ENOSPC or -EFBIG error is more likely for a sparse index, which needs
   * to store 10 times as many chapters as the equivalent dense index.
   *
   * Our lab systems have sufficient resources to always create a normal index.
   * Normal for our tests means a 1GB dense index or a 0.25GB sparse index.
   * When we run the test on hosts supplied by beaker, there is no telling what
   * will happen.  If there is a problem on the acceptable list in creating a
   * normal index, this test will pass.  But other tests in the checkin or
   * jenkins suites will certainly fail.
   *
   * Another possible failure occurs when some other process tries to allocate
   * memory at the same time as this test is running.  While UDS asks for
   * memory without invoking the oom killer, this other process may cause the
   * oom killer to run and kill the albtest process.  It is difficult to avoid
   * this problem in user mode tests.  This assertion will succeed, but the
   * test system will see the oom killer messages in the kernel log and fail
   * the test run.
   */
  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  int result = uds_open_index(UDS_CREATE, &params, indexSession);
  UDS_ASSERT_ERROR4(UDS_SUCCESS, -ENOMEM, -ENOSPC, -EFBIG, result);
  if (result == UDS_SUCCESS) {
    /*
     * Test that the geometry has a usable chapter index.  We will write
     * 16 chapters and make sure that the chapter index is built without
     * discarding any entries.
     */
    initializeOldInterfaces(1000);

    // Fill 16 chapters
    uint64_t counter = 0;
    int numChapters = 16;
    int chapter;
    for (chapter = 0; chapter < numChapters; chapter++) {
      unsigned int n;
      for (n = 0; n < config->geometry->records_per_chapter; n++) {
        struct uds_record_name chunkName = murmurHashChunkName(&counter,
							       sizeof(counter),
							       0);
        oldPostBlockName(indexSession, NULL,
                         (struct uds_chunk_data *) &chunkName,
                         &chunkName, cbStatus);
        counter++;
      }
    }
    UDS_ASSERT_SUCCESS(uds_flush_index_session(indexSession));

    // Test that the memory usage is as expected
    struct uds_index_stats stats;
    UDS_ASSERT_SUCCESS(uds_get_index_stats(indexSession, &stats));
    uds_log_info("Using %llu bytes of %zu",
                 (unsigned long long) stats.memory_used, tc->memoryUsed);
    CU_ASSERT(100 * stats.memory_used <= 102 * tc->memoryUsed);
    UDS_ASSERT_SUCCESS(uds_close_index(indexSession));

    uninitializeOldInterfaces();
  }
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  free_configuration(config);

  // Make sure the chapter index did not throw away any entries by an
  // unexpected discard or an overflow.
  CU_ASSERT_EQUAL(chapter_index_empty_count, chapter_index_discard_count);
  CU_ASSERT_EQUAL(0, chapter_index_overflow_count);
}

/**********************************************************************/
static void testSparse(TestConfig *tc)
{
  tc->sparse = true;
  tc->chaptersPerVolume *= 10;
  testRun(tc);
}

/**********************************************************************/
static void mb256Test(void)
{
  TestConfig tc;
  tc.memGB                 = UDS_MEMORY_CONFIG_256MB;
  tc.sparse                = false;
  tc.recordPagesPerChapter = SMALL_RECORD_PAGES_PER_CHAPTER;
  tc.chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
  tc.indexPagesPerChapter  = 6;
  tc.memoryUsed            = 256L * MEGABYTE;
  testRun(&tc);
  testSparse(&tc);
}

/**********************************************************************/
static void mb512Test(void)
{
  TestConfig tc;
  tc.memGB                 = UDS_MEMORY_CONFIG_512MB;
  tc.sparse                = false;
  tc.recordPagesPerChapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
  tc.chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
  tc.indexPagesPerChapter  = 13;
  tc.memoryUsed            = 512L * MEGABYTE;
  testRun(&tc);
  testSparse(&tc);
}

/**********************************************************************/
static void mb768Test(void)
{
  TestConfig tc;
  tc.memGB                 = UDS_MEMORY_CONFIG_768MB;
  tc.sparse                = false;
  tc.recordPagesPerChapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
  tc.chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
  tc.indexPagesPerChapter  = 20;
  tc.memoryUsed            = 768L * MEGABYTE;
  testRun(&tc);
  testSparse(&tc);
}

/**********************************************************************/
static void gb1Test(void)
{
  TestConfig tc;
  tc.memGB                 = 1;
  tc.sparse                = false;
  tc.recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
  tc.chaptersPerVolume     = DEFAULT_CHAPTERS_PER_VOLUME;
  tc.indexPagesPerChapter  = 26;
  tc.memoryUsed            = 1L * GIGABYTE;
  testRun(&tc);
  testSparse(&tc);
}

/**********************************************************************/
static void gb2Test(void)
{
  size_t memTotal = getMemTotalInGB();
  if (memTotal >= 2) {
    TestConfig tc;
    tc.memGB                 = 2;
    tc.sparse                = false;
    tc.recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
    tc.chaptersPerVolume     = 2 * DEFAULT_CHAPTERS_PER_VOLUME;
    tc.indexPagesPerChapter  = 26;
    tc.memoryUsed            = 2L * GIGABYTE;
    testRun(&tc);
    testSparse(&tc);
  }
}

/**********************************************************************/
static void bigTest(void)
{
  /*
   * Trying to use all the memory often produces an inappropriate
   * configuration, so limit this case to something which should fit
   * on any reasonable test machine.
   */
  size_t memTotal = min((size_t) 8, getMemTotalInGB());
  if (memTotal > 2) {
    TestConfig tc;
    tc.memGB                 = memTotal;
    tc.sparse                = false;
    tc.recordPagesPerChapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
    tc.chaptersPerVolume     = memTotal * DEFAULT_CHAPTERS_PER_VOLUME;
    tc.indexPagesPerChapter  = 26;
    tc.memoryUsed            = memTotal * GIGABYTE;
    testRun(&tc);
  }
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Saved", savedTest },
  { "256MB", mb256Test },
  { "512MB", mb512Test },
  { "768MB", mb768Test },
  { "1GB",   gb1Test },
  { "2GB",   gb2Test },
  { "Big",   bigTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Configuration_n1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
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
