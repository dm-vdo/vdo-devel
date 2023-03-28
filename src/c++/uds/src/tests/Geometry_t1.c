// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "geometry.h"
#include "testPrototypes.h"

/**********************************************************************/
static void checkSparsenessAndDensity(const struct geometry *g,
                                      bool expectSparse)
{
  CU_ASSERT_EQUAL(is_sparse_geometry(g), expectSparse);
  CU_ASSERT_EQUAL(g->dense_chapters_per_volume,
                  g->chapters_per_volume - g->sparse_chapters_per_volume);
}

/**********************************************************************/
static void checkCommonGeometry(const struct geometry *g,
                                unsigned int           chapters_per_volume)
{
  CU_ASSERT_EQUAL(g->bytes_per_page,      DEFAULT_BYTES_PER_PAGE);
  CU_ASSERT_EQUAL(g->chapters_per_volume, chapters_per_volume);
  CU_ASSERT_EQUAL(g->bytes_per_volume,
                  g->bytes_per_page * (g->pages_per_volume + HEADER_PAGES_PER_VOLUME));
  CU_ASSERT_EQUAL(g->records_per_page, g->bytes_per_page / BYTES_PER_RECORD);
  CU_ASSERT_EQUAL(g->chapter_address_bits, 22);
  CU_ASSERT_EQUAL(g->chapter_mean_delta, 1 << 16);

  CU_ASSERT_EQUAL(g->sparse_chapters_per_volume,
                  DEFAULT_SPARSE_CHAPTERS_PER_VOLUME);
  CU_ASSERT_EQUAL(g->pages_per_chapter,
                  g->index_pages_per_chapter + g->record_pages_per_chapter);
  CU_ASSERT_EQUAL(g->pages_per_volume,
                  g->chapters_per_volume * g->pages_per_chapter);
  CU_ASSERT_EQUAL(g->records_per_volume,
                  g->records_per_chapter * g->chapters_per_volume);
}

/**********************************************************************/
static void checkDefaultGeometry(struct geometry *g,
                                 unsigned int     chapters_per_volume)
{
  unsigned int indexPagesPerChapter = 26;

  CU_ASSERT_EQUAL(g->record_pages_per_chapter,
                  DEFAULT_RECORD_PAGES_PER_CHAPTER);
  CU_ASSERT_EQUAL(g->chapter_delta_list_bits,
                  DEFAULT_CHAPTER_DELTA_LIST_BITS);
  CU_ASSERT_EQUAL(g->chapter_payload_bits,    8);
  CU_ASSERT_EQUAL(g->index_pages_per_chapter,  indexPagesPerChapter);
  CU_ASSERT_EQUAL(g->delta_lists_per_chapter,  1 << 12);
  checkCommonGeometry(g, chapters_per_volume);
  checkSparsenessAndDensity(g, false);
}

/**********************************************************************/
static void testDefault(void)
{
  /* Test default 1024 chapters/volume */
  struct configuration *config = makeDenseConfiguration(1);
  struct geometry *g = config->geometry;
  checkDefaultGeometry(g, DEFAULT_CHAPTERS_PER_VOLUME);
  /**
   * Verify that this geometry allows indexing 1TB of 4K blocks.
   **/
  CU_ASSERT_EQUAL(g->records_per_volume, 256 * 1024 * 1024);

  free_configuration(config);
}

/**********************************************************************/
static void testDefaultReduced(void)
{
  /* Test 1023 chapters/volume, such as VDO would create, if it had to
   * re-create an index that had been converted to 1023 chapters/volume
   */
  struct configuration *config
    = makeDenseConfiguration(1 | UDS_MEMORY_CONFIG_REDUCED);
  struct geometry *g = config->geometry;
  checkDefaultGeometry(g, DEFAULT_CHAPTERS_PER_VOLUME - 1);
  /**
   * Verify that this geometry allows indexing 1TB of 4K blocks minus
   * one chapter's worth.
   **/
  CU_ASSERT_EQUAL(g->records_per_volume, 256 * 1024 * 1023);

  free_configuration(config);
}

/**********************************************************************/
static void checkSmallGeometry(struct geometry *g,
                                 unsigned int   chapters_per_volume)
{
  unsigned int indexPagesPerChapter = 6;

  CU_ASSERT_EQUAL(g->record_pages_per_chapter, SMALL_RECORD_PAGES_PER_CHAPTER);
  CU_ASSERT_EQUAL(g->chapter_delta_list_bits,  SMALL_CHAPTER_DELTA_LIST_BITS);
  CU_ASSERT_EQUAL(g->chapter_payload_bits,     6);
  CU_ASSERT_EQUAL(g->index_pages_per_chapter,  indexPagesPerChapter);
  CU_ASSERT_EQUAL(g->delta_lists_per_chapter,  1 << 10);

  checkCommonGeometry(g, chapters_per_volume);
  checkSparsenessAndDensity(g, false);
}

/**********************************************************************/
static void testSmall(void)
{
  struct configuration *config
    = makeDenseConfiguration(UDS_MEMORY_CONFIG_256MB);
  struct geometry *g = config->geometry;

  checkSmallGeometry(g, DEFAULT_CHAPTERS_PER_VOLUME);
  /**
   * Verify that this geometry allows indexing 256GB of 4K blocks
   * minus one chapter's worth.
   **/
  CU_ASSERT_EQUAL(g->records_per_volume, 64 * 1024 * 1024);

  free_configuration(config);
}

/**********************************************************************/
static void testSmallReduced(void)
{
  struct configuration *config
    = makeDenseConfiguration(UDS_MEMORY_CONFIG_REDUCED_256MB);
  struct geometry *g = config->geometry;

  checkSmallGeometry(g, DEFAULT_CHAPTERS_PER_VOLUME - 1);
  /**
   * Verify that this geometry allows indexing 256GB of 4K blocks
   * minus one chapter's worth.
   **/
  CU_ASSERT_EQUAL(g->records_per_volume, 64 * 1024 * 1023);

  free_configuration(config);
}

/**********************************************************************/
static void checkComputations(bool sparse)
{
  struct geometry *geometry;
  unsigned int chapters = 10;
  unsigned int sparseChapters = (sparse ? 5 : 0);
  UDS_ASSERT_SUCCESS(make_geometry(1024, 1, chapters, sparseChapters, 0, 0,
                                   &geometry));
  checkSparsenessAndDensity(geometry, sparse);

  uint64_t chapter, newest, oldest;
  for (oldest = 0; oldest < chapters; oldest++) {
    for (newest = oldest; newest < chapters; newest++) {
      uint64_t active = newest - oldest + 1;
      bool hasSparse = has_sparse_chapters(geometry, oldest, newest);
      CU_ASSERT_EQUAL(hasSparse,
                      (active > geometry->dense_chapters_per_volume));
      for (chapter = oldest; chapter <= newest; chapter++) {
        bool shouldBeSparse
          = (hasSparse
             && (chapter <= newest - geometry->dense_chapters_per_volume));
        CU_ASSERT_EQUAL(is_chapter_sparse(geometry, oldest, newest, chapter),
                        shouldBeSparse);
      }
    }
  }
  free_geometry(geometry);
}

/**********************************************************************/
static void testDenseComputations(void)
{
  checkComputations(false);
}

/**********************************************************************/
static void testSparseComputations(void)
{
  checkComputations(true);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Default",            testDefault            },
  { "Small",              testSmall              },
  { "Default Reduced",    testDefaultReduced     },
  { "Small Reduced",      testSmallReduced       },
  { "DenseComputations",  testDenseComputations  },
  { "SparseComputations", testSparseComputations },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Geometry_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
