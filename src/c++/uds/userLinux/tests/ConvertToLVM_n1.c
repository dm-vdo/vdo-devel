/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/**
 * Test the function that moves a chapter to free up space that VDO
 * can use to allow for LVM metadata in front of the VDO data.
 */

#include "albtest.h"
#include "assertions.h"
#include "convertToLVM.h"
#include "fileUtils.h"
#include "geometry.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "testRequests.h"
#include "uds.h"

static const char *indexName;
static uint64_t nameCounter = 0;

enum {
  LVM_OFFSET = 512 * UDS_BLOCK_SIZE,
};

/**********************************************************************/
static unsigned long records_per_chapter(void)
{
  return (SMALL_RECORD_PAGES_PER_CHAPTER * DEFAULT_RECORDS_PER_PAGE);
}

/**********************************************************************/
static uint64_t fillIndex(struct uds_index_session *session,
                          unsigned long             record_count)
{
  uint64_t nameSeed = nameCounter;
  unsigned long i;

  for (i = 0; i < record_count; i++) {
    struct uds_record_name chunkName
      = murmurHashChunkName(&nameCounter, sizeof(nameCounter), 0);

    nameCounter++;
    oldPostBlockName(session, NULL, (struct uds_chunk_data *) &chunkName,
                     &chunkName, cbStatus);
  }
  UDS_ASSERT_SUCCESS(uds_flush_index_session(session));
  return nameSeed;
}

/**********************************************************************/
static void verifyData(struct uds_index_session *session,
                       unsigned long             record_count,
                       uint64_t                  nameSeed,
                       bool                      sparse)
{
  struct uds_index *index = session->index;
  struct uds_request request = { .type = UDS_QUERY_NO_UPDATE };
  unsigned long i;

  for (i = 0; i < record_count; i++) {
    request.record_name = murmurHashChunkName(&nameSeed, sizeof(nameSeed), 0);
    nameSeed++;

    if (sparse) {
      // just verify the hooks for simplicity
      bool hook = is_volume_index_sample(index->volume_index,
                                         &request.record_name);
      if (!hook) {
        continue;
      }
    }
    verify_test_request(index, &request, true, NULL);
  }
}

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  indexName = name;
}

/**********************************************************************/
static void slide_file(off_t bytes)
{
  enum {
    BUFFER_SIZE  = 4096,
  };
  int fd;
  void *buf;
  off_t offset;
  off_t file_size;
  size_t length;

  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(BUFFER_SIZE, byte, "buffer", &buf));
  UDS_ASSERT_SUCCESS(open_file(indexName, FU_READ_WRITE, &fd));

  UDS_ASSERT_SUCCESS(get_open_file_size(fd, &file_size));
  file_size = min(file_size, bytes);

  for (offset = LVM_OFFSET; offset < file_size; offset += BUFFER_SIZE) {
    UDS_ASSERT_SUCCESS(read_data_at_offset(fd, offset,
                                           buf, BUFFER_SIZE, &length));
    UDS_ASSERT_SUCCESS(write_buffer_at_offset(fd, offset - LVM_OFFSET,
                                              buf, length));
  }
  UDS_ASSERT_SUCCESS(sync_and_close_file(fd, "file copy"));
  UDS_FREE(UDS_FORGET(buf));
}

/**********************************************************************/
static void doTestCase(unsigned long record_count1,
                       unsigned long record_count2,
                       unsigned long record_count3,
                       bool sparse)
{
  struct uds_index_session *session;
  uint64_t index_size = 0;
  uint64_t nonce = 0xdeadface;
  uint64_t seed1 = 0;
  uint64_t seed2 = 0;
  uint64_t seed3 = 0;
  off_t start = 2 * 4096;       // Start two blocks in, like VDO
  off_t moved = 0;

  initializeOldInterfaces(2000);
  initialize_test_requests();

  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
    .nonce = nonce,
    .offset = start,
    .sparse = sparse,
  };
  UDS_ASSERT_SUCCESS(uds_compute_index_size(&params, &index_size));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  seed1 = fillIndex(session, record_count1);
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));

  UDS_ASSERT_SUCCESS(uds_convert_to_lvm(&params, LVM_OFFSET, &moved));
  slide_file(index_size);

  struct uds_parameters params2 = {
    .memory_size = params.memory_size,
    .name = indexName,
    .nonce = nonce,
    .offset = start + moved - LVM_OFFSET,
    .sparse = sparse,
  };

  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params2, session));
  verifyData(session, record_count1, seed1, sparse);

  seed2 = fillIndex(session, record_count2);
  verifyData(session, record_count2, seed2, sparse);

  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));

  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params2, session));
  verifyData(session, record_count1, seed1, sparse);
  verifyData(session, record_count2, seed2, sparse);

  // Verify that it is possible to add new records.
  seed3 = fillIndex(session, record_count3);
  verifyData(session, record_count1, seed1, sparse);
  verifyData(session, record_count2, seed2, sparse);
  verifyData(session, record_count3, seed3, sparse);
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
  uninitialize_test_requests();
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void emptyTest(void)
{
  unsigned long records = 0;
  doTestCase(records, 1, 1, false);
}

/************************************************************************/
static void oneRecordTest(void)
{
  unsigned long records = 1;
  doTestCase(records, records, 1, false);
}

/************************************************************************/
static void oneRecordSparseTest(void)
{
  unsigned long records = 1;
  doTestCase(records, records, 1, true);
}

/************************************************************************/
static void oneChapterTest(void)
{
  unsigned long records = records_per_chapter();
  doTestCase(records, records, 1, false);
}

/************************************************************************/
static void oneChapterPlusOneTest(void)
{
  unsigned long records = records_per_chapter();
  doTestCase(records + 1, records + 1, 1, false);
}

/************************************************************************/
static void twoChapterTest(void)
{
  unsigned long records = 2UL * records_per_chapter();
  doTestCase(records, records, 1, false);
}

/************************************************************************/

static const CU_TestInfo tests[] = {
  { "convertEmpty",             emptyTest },
  { "convertOneRecord",         oneRecordTest },
  { "convertOneChapter",        oneChapterTest },
  { "convertOneChapterPlusOne", oneChapterPlusOneTest },
  { "convertTwoChapter",        twoChapterTest },
  { "oneRecordSparse",          oneRecordSparseTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "ConvertToLVM_n1",
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
