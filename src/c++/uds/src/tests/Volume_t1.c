// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "random.h"
#include "testPrototypes.h"
#include "volume.h"
#include "volumeUtils.h"

static struct index_layout  *layout;
static struct configuration *config;
static struct geometry      *geometry;
static byte                **pages;
static struct volume        *volume;

/**********************************************************************/
static void init(const char *indexName)
{
  // Pages need to be large enough for full header (which is the version
  // string plus the geometry, which is currently 88 bytes.  And also large
  // enough to make the storage device happy.
  struct uds_parameters params = {
    .memory_size = 1,
    .name = indexName,
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  resizeDenseConfiguration(config, 4096, 8, 128);
  UDS_ASSERT_SUCCESS(make_uds_index_layout(config, true, &layout));

  UDS_ASSERT_SUCCESS(make_volume(config, layout, &volume));

  geometry = config->geometry;
  pages = makePageArray(geometry->pages_per_volume, geometry->bytes_per_page);
  writeTestVolumeData(volume, geometry, pages);
}

/**********************************************************************/
static void deinit(void)
{
  freePageArray(pages, geometry->pages_per_volume);
  free_volume(volume);
  free_configuration(config);
  free_uds_index_layout(UDS_FORGET(layout));
}

/**********************************************************************/
static void verifyPage(unsigned int chapter, unsigned int page)
{
  uint32_t physPage = chapter * geometry->pages_per_chapter + page;
  const byte *expected = pages[physPage];
  byte *actual;
  // Make sure the page read is synchronous
  UDS_ASSERT_SUCCESS(get_volume_page(volume, chapter, page, &actual, NULL));
  UDS_ASSERT_EQUAL_BYTES(actual, expected, geometry->bytes_per_page);
}

/**********************************************************************/
static void testSequentialGet(void)
{
  unsigned int chapter, page;
  for (chapter = 0; chapter < geometry->chapters_per_volume; ++chapter) {
    for (page = 0; page < geometry->pages_per_chapter; ++page) {
      verifyPage(chapter, page);
    }
  }
}

/**********************************************************************/
static void testStumblingGet(void)
{
  unsigned int page;
  for (page = 0; page < geometry->pages_per_volume;) {
    unsigned int chapter = page / geometry->pages_per_chapter;
    unsigned int relPage = page % geometry->pages_per_chapter;
    verifyPage(chapter, relPage);
    // back one page 25%, same page 25%, forward one page 50%.
    unsigned int action = random() % 4;
    if (action == 0) {
      if (page > 0) {
	--page;
      }
    } else if (action != 1) {
      ++page;
    }
  }
}

/**********************************************************************/
static void testWriteChapter(void)
{
  // XXX this test only exercises the write code, and does nothing to check
  // that it does anything correctly.
  struct uds_chunk_record *records;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1 + geometry->records_per_chapter,
                                  struct uds_chunk_record, __func__,
                                  &records));

  fill_randomly((byte *) records,
                BYTES_PER_RECORD * (1 + geometry->records_per_chapter));

  // Construct an empty delta chapter index for chapter zero. The chapter
  // write code doesn't really care if it's populated or not.
  struct open_chapter_index *chapterIndex;
  UDS_ASSERT_SUCCESS(make_open_chapter_index(&chapterIndex, geometry,
                                             volume->nonce));
  CU_ASSERT_PTR_NOT_NULL(chapterIndex);

  // Write chapter zero.
  empty_open_chapter_index(chapterIndex, 0);
  UDS_ASSERT_SUCCESS(write_chapter(volume, chapterIndex, records));

  // Write an empty delta chapter index for chapter one.
  empty_open_chapter_index(chapterIndex, 1);
  UDS_ASSERT_SUCCESS(write_chapter(volume, chapterIndex, records));
  free_open_chapter_index(chapterIndex);

  UDS_FREE(records);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"SequentialGet",        testSequentialGet},
  {"StumblingGet",         testStumblingGet},
  {"WriteChapter",         testWriteChapter},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Volume_t1",
  .initializerWithIndexName = init,
  .cleaner                  = deinit,
  .tests                    = tests,
};

// =============================================================================
// Entry point required by the module loader. Return a pointer to the
// const CU_SuiteInfo structure.
// =============================================================================

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
