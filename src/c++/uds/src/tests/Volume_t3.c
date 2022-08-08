// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "volume.h"

struct auxData {
  const uint64_t *data;
  size_t          length;
};

static struct geometry *geometry;

/**********************************************************************/
static int myProbe(void *aux, unsigned int chapter, uint64_t *vcn)
{
  struct auxData *ad = (struct auxData *) aux;

  if (chapter >= ad->length) {
    return UDS_OUT_OF_RANGE;
  }

  *vcn = ad->data[chapter];
  return UDS_SUCCESS;
}

/**********************************************************************/
static void testFindBoundaries(int             expectedResult,
                               uint64_t        expectedLowest,
                               uint64_t        expectedHighest,
                               const uint64_t *data,
                               size_t          size)
{
  uint64_t lowest  = UINT64_MAX - 2;
  uint64_t highest = UINT64_MAX - 1;

  unsigned int chapterLimit = size / sizeof(uint64_t);

  struct auxData auxData = {
    .data = data,
    .length = chapterLimit,
  };

  unsigned int maxBadChapters = 8;

  if (expectedResult == UDS_CORRUPT_DATA) {
    // use shorter max bad to save typing in test data
    maxBadChapters = 2;
  } else if (expectedResult == UDS_OUT_OF_RANGE) {
    // force probe function to get illegal chapter number by lying about
    // the number of chapters
    chapterLimit *= 3;
  }

  UDS_ASSERT_ERROR(expectedResult,
                   find_volume_chapter_boundaries_impl(chapterLimit,
                                                       maxBadChapters, &lowest,
                                                       &highest, myProbe,
                                                       geometry,
                                                       &auxData));
  if (expectedResult == UDS_SUCCESS) {
    CU_ASSERT_EQUAL(lowest, expectedLowest);
    CU_ASSERT_EQUAL(highest, expectedHighest);
  } else {
    CU_ASSERT_EQUAL(lowest, UINT64_MAX - 2);
    CU_ASSERT_EQUAL(highest, UINT64_MAX - 1);
  }
}

/**********************************************************************/
static void findBoundariesTest(void)
{
  UDS_ASSERT_SUCCESS(make_geometry(DEFAULT_BYTES_PER_PAGE,
                                   DEFAULT_RECORD_PAGES_PER_CHAPTER,
                                   DEFAULT_CHAPTERS_PER_VOLUME,
                                   DEFAULT_SPARSE_CHAPTERS_PER_VOLUME,
                                   0, 0, &geometry));

  static const uint64_t data1[] = { 0, 1, 2, 3 };
  testFindBoundaries(UDS_SUCCESS, 0, 3, data1, sizeof(data1));

  static const uint64_t data2[] = { UINT64_MAX, UINT64_MAX, 2, 3, 4 };
  testFindBoundaries(UDS_SUCCESS, 2, 4, data2, sizeof(data2));

  static const uint64_t data3[] = { UINT64_MAX, 1, 2, 3, UINT64_MAX,
                                    UINT64_MAX };
  testFindBoundaries(UDS_SUCCESS, 1, 3, data3, sizeof(data3));

  static const uint64_t data4[] = { 10, 11, 12, 13, UINT64_MAX, UINT64_MAX,
                                    UINT64_MAX, UINT64_MAX, 8, 9 };
  testFindBoundaries(UDS_SUCCESS, 8, 13, data4, sizeof(data4));

  static const uint64_t data5[] = { 10, 11, 12, 13, 14, 15, 6, 7, 8, 9 };
  testFindBoundaries(UDS_SUCCESS, 6, 15, data5, sizeof(data5));

  static const uint64_t data6[] = { 30, 31, 32, 33, 34, 35, 36, 37, UINT64_MAX,
                                    UINT64_MAX };
  testFindBoundaries(UDS_SUCCESS, 30, 37, data6, sizeof(data6));

  static const uint64_t data7[] = { 30, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                                    UINT64_MAX, UINT64_MAX, UINT64_MAX, 27, 28,
                                    29 };
  testFindBoundaries(UDS_SUCCESS, 27, 30, data7, sizeof(data7));

  static const uint64_t data8[] = { UINT64_MAX, UINT64_MAX, UINT64_MAX,
                                    4, 5, 6, 7, 8, 9, 10 };
  testFindBoundaries(UDS_CORRUPT_DATA, 0, 0, data8, sizeof(data8));

  static const uint64_t data9[] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                    UINT64_MAX, UINT64_MAX, UINT64_MAX };
  testFindBoundaries(UDS_CORRUPT_DATA, 0, 0, data9, sizeof(data9));

  static const uint64_t data10[] = { 0, 1, 2, 3 };
  testFindBoundaries(UDS_OUT_OF_RANGE, 0, 0, data10, sizeof(data10));

  static const uint64_t data11[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 9 };
  testFindBoundaries(UDS_SUCCESS, 9, 18, data11, sizeof(data11));

  static const uint64_t data12[] = { 10, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  testFindBoundaries(UDS_SUCCESS, 1, 10, data12, sizeof(data12));

  UDS_FREE(UDS_FORGET(geometry));
}

/**********************************************************************/
static void findConvertedBoundariesTest(void)
{
  // Make a tiny geometry with a remapped chapter
  UDS_ASSERT_SUCCESS(make_geometry(DEFAULT_BYTES_PER_PAGE,
                                   DEFAULT_RECORD_PAGES_PER_CHAPTER,
                                   7,
                                   DEFAULT_SPARSE_CHAPTERS_PER_VOLUME,
                                   8, 2, &geometry));

  static const uint64_t data1[] = { 9, 10, 8, 4, 5, 6, 7 };
  testFindBoundaries(UDS_SUCCESS, 4, 10, data1, sizeof(data1));

  static const uint64_t data2[] = { 9, 10, 8, UINT64_MAX, UINT64_MAX, 6, 7 };
  testFindBoundaries(UDS_SUCCESS, 6, 10, data2, sizeof(data2));

  static const uint64_t data3[] = { 9, 10, 8, 11, 5, 6, 7 };
  testFindBoundaries(UDS_SUCCESS, 5, 11, data3, sizeof(data3));

  static const uint64_t data4[] = { 9, 10, 8, 11, UINT64_MAX, UINT64_MAX, 7 };
  testFindBoundaries(UDS_SUCCESS, 7, 11, data4, sizeof(data4));

  static const uint64_t data5[] = { 9, 10, 8, 11, 12, 6, 7 };
  testFindBoundaries(UDS_SUCCESS, 6, 12, data5, sizeof(data5));

  static const uint64_t data6[] = { 9, 10, 8, 11, 12, UINT64_MAX, UINT64_MAX };
  testFindBoundaries(UDS_SUCCESS, 8, 12, data6, sizeof(data6));

  static const uint64_t data7[] = { 9, 10, 8, 11, 12, 13, 7 };
  testFindBoundaries(UDS_SUCCESS, 7, 13, data7, sizeof(data7));

  static const uint64_t data8[] = { UINT64_MAX, 10, 8, 11, 12, 13,
                                    UINT64_MAX };
  testFindBoundaries(UDS_SUCCESS, 10, 13, data8, sizeof(data8));

  static const uint64_t data9[] = { 9, 10, 8, 11, 12, 13, 14 };
  testFindBoundaries(UDS_SUCCESS, 8, 14, data9, sizeof(data9));

  static const uint64_t data10[] = { UINT64_MAX, UINT64_MAX, 8, 11, 12, 13,
                                     14 };
  testFindBoundaries(UDS_SUCCESS, 11, 14, data10, sizeof(data10));

  static const uint64_t data11[] = { 15, 10, 8, 11, 12, 13, 14 };
  testFindBoundaries(UDS_SUCCESS, 10, 15, data11, sizeof(data11));

  static const uint64_t data12[] = { 15, 16, 8, 11, 12, 13, 14 };
  testFindBoundaries(UDS_SUCCESS, 11, 16, data12, sizeof(data12));

  static const uint64_t data13[] = { 15, 16, UINT64_MAX, UINT64_MAX, 12, 13,
                                    14 };
  testFindBoundaries(UDS_SUCCESS, 12, 16, data13, sizeof(data13));

  static const uint64_t data14[] = { 15, 16, 17, 11, 12, 13, 14 };
  testFindBoundaries(UDS_SUCCESS, 11, 17, data14, sizeof(data14));

  static const uint64_t data15[] = { 15, 16, 17, UINT64_MAX, UINT64_MAX, 13,
                                     14 };
  testFindBoundaries(UDS_SUCCESS, 13, 17, data15, sizeof(data15));

  UDS_FREE(UDS_FORGET(geometry));
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
