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
#include "blockTestUtils.h"
#include "config.h"
#include "convertToLVM.h"
#include "fileUtils.h"
#include "hash-utils.h"
#include "numeric.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"
#include "testRequests.h"
#include "uds.h"

static const char *indexName;

enum {
  /*
   * The number of address bits is computed, down in the library where
   * it would be hard to extract, but from values that currently can't
   * be changed from the defaults, so it's effectively a constant.
   */
  ADDRESS_BITS = 20,

  /*
   * How much space the LVM conversion steals from the start of the
   * index.
   */
  LVM_OFFSET = 512 * UDS_BLOCK_SIZE,

  /*
   * The more zones we use, the fewer records we need to write in
   * order to "fill" one chapter and move on to the next.
   */
  ZONES = MAX_ZONES,
};

// Number of delta lists is retrieved; lists per zone is computed.
struct index_info {
  unsigned int num_delta_lists;
  unsigned int lists_per_zone;
} dense_info, sparse_info;

static uint64_t nameCounter;
// geometry
uint64_t records_per_chapter;   // assumes filling only one zone of N
unsigned int num_chapters;      // changes on conversion
// total written so far
unsigned long total_records;
// conversion state
bool converted;
unsigned int chapters_written_at_conversion;
// working out what we've forgotten
unsigned int chapters_written_so_far;
unsigned int forgotten_chapters;
// and what we still remember
unsigned int active_chapters;
// test state
struct uds_index_session *session;
struct uds_parameters uds_parameters = { .zone_count = ZONES };

/**********************************************************************/
static void compute_index_info(struct volume_index *volumeIndex)
{
  struct volume_index_stats *volumeStats;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(2, struct volume_index_stats, __func__,
                                  &volumeStats));
  get_volume_index_stats(volumeIndex, volumeStats, volumeStats+1);
  dense_info.num_delta_lists = volumeStats[0].num_lists;
  sparse_info.num_delta_lists = volumeStats[1].num_lists;
  UDS_FREE(volumeStats);
  // deltaIndex.c:initialize_delta_index
  dense_info.lists_per_zone = (dense_info.num_delta_lists + ZONES - 1) / ZONES;
  sparse_info.lists_per_zone =
    (sparse_info.num_delta_lists + ZONES - 1) / ZONES;
}

/**
 * Recalculate the derived values chapters_written_so_far,
 * forgotten_chapters, and active_chapters from those describing the
 * index geometry or test progress (total_records,
 * records_per_chapter, num_chapters, converted,
 * chapters_written_at_conversion). We don't try to update previous
 * values, just recalculate them from scratch.
 *
 * Officially no inputs or output values in the function signature to
 * document, but the global variables include several input and output
 * values.
 **/
static void recalculate_stats(void)
{
  chapters_written_so_far = total_records / records_per_chapter;
  if (chapters_written_so_far >= num_chapters) {
    forgotten_chapters = chapters_written_so_far - (num_chapters - 1);
    // conversion forgets an extra chapter for a while
    if (converted &&
        // When (new) num_chapters is 1023, 0..1022 new chapters means
        // we may not have reached the new normal yet, but
        // chapters_written_at_conversion+1023 means we've definitely
        // written every chapter in the converted index *since*
        // conversion, and thus we're in the new-normal mode.
        (chapters_written_so_far <
         (chapters_written_at_conversion + num_chapters))) {
      forgotten_chapters++;
    }
  } else {
    forgotten_chapters = 0;
  }
  active_chapters = chapters_written_so_far - forgotten_chapters;
}

/**
 * Alter a chunk name in place so that the volume zone used will be
 * zone 0, regardless of the number of zones configured.
 *
 * @param index  The volume index
 * @param name   The chunk name to be altered
 **/
static void adjust_list_number_for_zone_0(struct volume_index   *index,
                                          struct uds_chunk_name *name)
{
  unsigned int zone = get_volume_index_zone(index, name);
  if (zone == 0) {
    return;
  }

  const struct index_info *index_info
    = (is_volume_index_sample(index, name) ? &sparse_info : &dense_info);
  unsigned int lists_per_zone = index_info->lists_per_zone;
  uint64_t bits = extract_volume_index_bytes(name);
  // Change, e.g., the 4th list of zone 3 to the 4th list of zone 0.
  // This simple decrement can't wrap.
  bits -= ((uint64_t)(zone * lists_per_zone)) << ADDRESS_BITS;
  set_volume_index_bytes(name, bits);

  // Sanity check
  CU_ASSERT_EQUAL(get_volume_index_zone(index, name), 0);
}

/**********************************************************************/
static void fillIndex(unsigned int chapter_count)
{
  struct uds_index *index = session->index;
  struct uds_request request = { .type = UDS_UPDATE };
  unsigned long i;
  unsigned long record_count = records_per_chapter * chapter_count;
  for (i = 0; i < record_count; i++) {
    request.chunk_name
      = murmurHashChunkName(&nameCounter, sizeof(nameCounter), 0);
    nameCounter++;
    adjust_list_number_for_zone_0(index->volume_index, &request.chunk_name);
    verify_test_request(index, &request, false, NULL);
  }
  index->need_to_save |= (record_count > 0);
  total_records += record_count;
  recalculate_stats();
}

/**********************************************************************/
static void verifyData(bool sparse)
{
  struct uds_index *index = session->index;
  struct uds_request request = { .type = UDS_QUERY_NO_UPDATE };
  unsigned long i;
  // For now, we always verify all of the records we think we should
  // currently remember. The first record written is #0, and they're
  // consecutively numbered.
  uint64_t first_record = forgotten_chapters * records_per_chapter;
  unsigned long record_count = active_chapters * records_per_chapter;

  for (i = 0; i < record_count; i++) {
    uint64_t recordNumber = first_record + i;
    request.chunk_name = murmurHashChunkName(&recordNumber,
					     sizeof(recordNumber), 0);
    adjust_list_number_for_zone_0(index->volume_index, &request.chunk_name);

    if (sparse) {
      // just verify the hooks for simplicity
      bool hook = is_volume_index_sample(index->volume_index,
                                         &request.chunk_name);
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
static void do_conversion(struct uds_parameters *params, off_t *start_p)
{
  uint64_t index_size;
  off_t moved = 0;
  UDS_ASSERT_SUCCESS(uds_compute_index_size(params, &index_size));
  UDS_ASSERT_SUCCESS(uds_convert_to_lvm(params, LVM_OFFSET, &moved));
  converted = true;
  chapters_written_at_conversion = chapters_written_so_far;
  num_chapters--; // should re-retrieve it from index...
  // update for new num_chapters
  recalculate_stats();
  slide_file(index_size);
  *start_p += moved - LVM_OFFSET;
}

/**********************************************************************/
static void do_fill_and_verify(struct uds_parameters *params,
                               unsigned int chapter_count,
                               bool do_close_and_reopen)
{
  fillIndex(chapter_count);
  verifyData(params->sparse);
  if (do_close_and_reopen) {
    UDS_ASSERT_SUCCESS(uds_close_index(session));
    UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));

    UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
    UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, params, session));
    verifyData(params->sparse);
  }
}

/**********************************************************************/
static void doTestCase(unsigned int chapter_count1, bool sparse)
{
  uint64_t nonce = 0xdeadface;
  off_t start = 2 * 4096;       // Start two blocks in, like VDO
  unsigned int chapter_count2;
  unsigned int chapter_count3;
  unsigned int i;

  // Reset non-calculated values:
  nameCounter = 0;
  records_per_chapter = 0;
  total_records = 0;
  num_chapters = 0;
  converted = false;
  chapters_written_at_conversion = 0;
  // and test state
  session = NULL;

  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
    .name = indexName,
    .nonce = nonce,
    .offset = start,
    .sparse = sparse,
    .zone_count = ZONES,
  };

  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, session));
  initialize_test_requests();
  // num_chapters is affected by the sparseness setting above.
  num_chapters = getChaptersPerIndex(session);
  records_per_chapter = getBlocksPerChapter(session) / ZONES;
  compute_index_info(session->index->volume_index);
  fillIndex(chapter_count1);

  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));

  do_conversion(&params, &start);

  struct uds_parameters params2 = {
    .memory_size = params.memory_size,
    .name = indexName,
    .nonce = nonce,
    .offset = start,
    .sparse = sparse,
    .zone_count = ZONES,
  };
  UDS_ASSERT_SUCCESS(uds_create_index_session(&session));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_NO_REBUILD, &params2, session));
  compute_index_info(session->index->volume_index);
  verifyData(sparse);

  /*
   * Next phase:
   *
   * A couple interesting cases where we might find problems in our
   * semi-black-box testing are where we wrap around to physical
   * chapter 0, and where we come back to the physical chapter we were
   * at when we did the conversion -- give or take a chapter or so in
   * both cases. Since these tests do their conversions in the
   * neighborhood of physical chapter 0, both are covered by examining
   * one range of chapters.
   *
   * If we were filling, say, 20 chapters out of 1024 and then
   * converting, we'd probably want to look at what happened when we
   * got in the neighborhood of 1023 filled, and then around
   * 1043. (And maybe 2046 and 2066? We should probably have at least
   * one test that does goes than one time around post-conversion.)
   * Supporting two different but possibly overlapping regions to
   * examine is a bit more complicated and isn't currently supported.
   *
   * We fill almost all the way around back to the current physical
   * chapter, save and reload the index, then fill chapter by chapter
   * with extensive verification until we wrap around past the same
   * physical chapter as the conversion point, and slightly beyond. We
   * keep saving and reloading the index during the chapter-by-chapter
   * portion.
   */
  chapter_count2 = num_chapters - 3UL;
  chapter_count3 = 6;

  do_fill_and_verify(&params2, chapter_count2, true);

  // Verify that it is possible to add new records and chapters at the
  // wraparound point.
  for (i = 0; i < chapter_count3; i++) {
    // Save and reload each time.
    do_fill_and_verify(&params2, 1, true);
  }
  uninitialize_test_requests();
  UDS_ASSERT_SUCCESS(uds_close_index(session));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(session));
}

/************************************************************************/
static void emptyTest(void)
{
  doTestCase(0, false);
}

/************************************************************************/
static void oneChapterTest(void)
{
  doTestCase(1, false);
}

/************************************************************************/
static void twoChaptersTest(void)
{
  doTestCase(2, false);
}

/************************************************************************/
static void twoChaptersSparseTest(void)
{
  doTestCase(2, true);
}

/************************************************************************/
static void fullMinusTwoChaptersTest(void)
{
  doTestCase(DEFAULT_CHAPTERS_PER_VOLUME - 2, false);
}

/************************************************************************/
static void fullMinusOneChapterTest(void)
{
  doTestCase(DEFAULT_CHAPTERS_PER_VOLUME - 1, false);
}

/************************************************************************/
static void fullTest(void)
{
  doTestCase(DEFAULT_CHAPTERS_PER_VOLUME, false);
}

/************************************************************************/
static void fullPlusOneChapterTest(void)
{
  doTestCase(DEFAULT_CHAPTERS_PER_VOLUME + 1, false);
}

/************************************************************************/
static void fullPlusTwoChaptersTest(void)
{
  doTestCase(DEFAULT_CHAPTERS_PER_VOLUME + 2, false);
}

// How about 2N +/- ?

/************************************************************************/

static const CU_TestInfo tests[] = {
  { "empty",                emptyTest },
  { "oneChapter",           oneChapterTest },
  { "twoChapters",          twoChaptersTest },
  { "twoChaptersSparse",    twoChaptersSparseTest },
  { "fullMinusTwoChapters", fullMinusTwoChaptersTest },
  { "fullMinusOneChapter",  fullMinusOneChapterTest },
  { "full",                 fullTest },
  { "fullPlusOneChapter",   fullPlusOneChapterTest },
  { "fullPlusTwoChapters",  fullPlusTwoChaptersTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "ConvertToLVM_x1",
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
