// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "volume.h"

static struct index_geometry *geometry;
struct volume volume;
static const u64 BAD_CHAPTER = U64_MAX;
const uint64_t *chapterData;

/**********************************************************************/
static void myProbe(u32 chapter, u64 *vcn)
{
  *vcn = chapterData[chapter];
}

/**********************************************************************/
static void testFindBoundaries(uint64_t        expectedLowest,
                               uint64_t        expectedHighest,
                               const uint64_t *data,
                               size_t          size)
{
  uint64_t lowest  = BAD_CHAPTER - 2;
  uint64_t highest = BAD_CHAPTER - 1;

  unsigned int chapterLimit = size / sizeof(u64);
  chapterData = data;
  UDS_ASSERT_SUCCESS(find_chapter_limits(&volume, chapterLimit, &lowest, &highest));
  CU_ASSERT_EQUAL(lowest, expectedLowest);
  CU_ASSERT_EQUAL(highest, expectedHighest);
}

/**********************************************************************/
static void findBoundariesTest(void)
{
  UDS_ASSERT_SUCCESS(uds_make_index_geometry(DEFAULT_BYTES_PER_PAGE,
                                             DEFAULT_RECORD_PAGES_PER_CHAPTER,
                                             DEFAULT_CHAPTERS_PER_VOLUME,
                                             DEFAULT_SPARSE_CHAPTERS_PER_VOLUME,
                                             0, 0, &geometry));
  volume.geometry = geometry;
  set_chapter_tester(myProbe);

  static const uint64_t data1[] = { 0, 1, 2, 3 };
  testFindBoundaries(0, 3, data1, sizeof(data1));

  static const uint64_t data2[] = { BAD_CHAPTER, BAD_CHAPTER, 2, 3, 4 };
  testFindBoundaries(2, 4, data2, sizeof(data2));

  static const uint64_t data3[] = { BAD_CHAPTER, 1, 2, 3, BAD_CHAPTER, BAD_CHAPTER };
  testFindBoundaries(1, 3, data3, sizeof(data3));

  static const uint64_t data4[] = { 10, 11, 12, 13, BAD_CHAPTER, BAD_CHAPTER, BAD_CHAPTER,
                                    BAD_CHAPTER, 8, 9 };
  testFindBoundaries(8, 13, data4, sizeof(data4));

  static const uint64_t data5[] = { 10, 11, 12, 13, 14, 15, 6, 7, 8, 9 };
  testFindBoundaries(6, 15, data5, sizeof(data5));

  static const uint64_t data6[] = { 30, 31, 32, 33, 34, 35, 36, 37, BAD_CHAPTER, BAD_CHAPTER };
  testFindBoundaries(30, 37, data6, sizeof(data6));

  static const uint64_t data7[] = { 30, BAD_CHAPTER, BAD_CHAPTER, BAD_CHAPTER, BAD_CHAPTER,
                                    BAD_CHAPTER, BAD_CHAPTER, 27, 28, 29 };
  testFindBoundaries(27, 30, data7, sizeof(data7));

  static const uint64_t data11[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 9 };
  testFindBoundaries(9, 18, data11, sizeof(data11));

  static const uint64_t data12[] = { 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  testFindBoundaries(1, 10, data12, sizeof(data12));

  set_chapter_tester(NULL);
  vdo_free(vdo_forget(geometry));
}

/**********************************************************************/
static void findConvertedBoundariesTest(void)
{
  // Remap a chapter in the middle
  UDS_ASSERT_SUCCESS(uds_make_index_geometry(DEFAULT_BYTES_PER_PAGE,
                                             DEFAULT_RECORD_PAGES_PER_CHAPTER,
                                             7,
                                             DEFAULT_SPARSE_CHAPTERS_PER_VOLUME,
                                             8, 2, &geometry));
  volume.geometry = geometry;
  set_chapter_tester(myProbe);

  static const uint64_t data1[] = { 9, 10, 8, 4, 5, 6, 7 };
  testFindBoundaries(4, 10, data1, sizeof(data1));

  static const uint64_t data2[] = { 9, 10, 8, BAD_CHAPTER, BAD_CHAPTER, 6, 7 };
  testFindBoundaries(6, 10, data2, sizeof(data2));

  static const uint64_t data3[] = { 9, 10, 8, 11, 5, 6, 7 };
  testFindBoundaries(5, 11, data3, sizeof(data3));

  static const uint64_t data4[] = { 9, 10, 8, 11, BAD_CHAPTER, BAD_CHAPTER, 7 };
  testFindBoundaries(7, 11, data4, sizeof(data4));

  static const uint64_t data5[] = { 9, 10, 8, 11, 12, 6, 7 };
  testFindBoundaries(6, 12, data5, sizeof(data5));

  static const uint64_t data6[] = { 9, 10, 8, 11, 12, BAD_CHAPTER, BAD_CHAPTER };
  testFindBoundaries(8, 12, data6, sizeof(data6));

  static const uint64_t data7[] = { 9, 10, 8, 11, 12, 13, 7 };
  testFindBoundaries(7, 13, data7, sizeof(data7));

  static const uint64_t data8[] = { BAD_CHAPTER, 10, 8, 11, 12, 13, BAD_CHAPTER };
  testFindBoundaries(10, 13, data8, sizeof(data8));

  static const uint64_t data9[] = { 9, 10, 8, 11, 12, 13, 14 };
  testFindBoundaries(8, 14, data9, sizeof(data9));

  static const uint64_t data10[] = { BAD_CHAPTER, BAD_CHAPTER, 8, 11, 12, 13, 14 };
  testFindBoundaries(11, 14, data10, sizeof(data10));

  static const uint64_t data11[] = { 15, 10, 8, 11, 12, 13, 14 };
  testFindBoundaries(10, 15, data11, sizeof(data11));

  static const uint64_t data12[] = { 15, 16, 8, 11, 12, 13, 14 };
  testFindBoundaries(11, 16, data12, sizeof(data12));

  static const uint64_t data13[] = { 15, 16, BAD_CHAPTER, BAD_CHAPTER, 12, 13, 14 };
  testFindBoundaries(12, 16, data13, sizeof(data13));

  static const uint64_t data14[] = { 15, 16, 17, 11, 12, 13, 14 };
  testFindBoundaries(11, 17, data14, sizeof(data14));

  static const uint64_t data15[] = { 15, 16, 17, BAD_CHAPTER, BAD_CHAPTER, 13, 14 };
  testFindBoundaries(13, 17, data15, sizeof(data15));
  vdo_free(vdo_forget(geometry));

  // Remapped a chapter to the end of the volume.
  UDS_ASSERT_SUCCESS(uds_make_index_geometry(DEFAULT_BYTES_PER_PAGE,
                                             DEFAULT_RECORD_PAGES_PER_CHAPTER,
                                             7,
                                             DEFAULT_SPARSE_CHAPTERS_PER_VOLUME,
                                             8, 6, &geometry));
  volume.geometry = geometry;

  static const uint64_t data16[] = { 9, 10, 11, 12, 13, 14, 8 };
  testFindBoundaries(8, 14, data16, sizeof(data16));

  static const uint64_t data17[] = { BAD_CHAPTER, 10, 11, 12, 13, 14, 8 };
  testFindBoundaries(10, 14, data17, sizeof(data17));

  static const uint64_t data18[] = { 15, 16, 11, 12, 13, 14, 8 };
  testFindBoundaries(11, 16, data18, sizeof(data18));
  set_chapter_tester(NULL);
  vdo_free(vdo_forget(geometry));
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "Find boundaries",           findBoundariesTest },
  { "Find converted boundaries", findConvertedBoundariesTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "Volume_t3",
  .tests = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
