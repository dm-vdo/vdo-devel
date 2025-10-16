// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "chapter-index.h"
#include "delta-index.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "testPrototypes.h"

enum { SAMPLE_CHAPTER_NUMBER = 0x65537 };
const uint64_t volumeNonce = 0x0123456789ABCDEF;

/**********************************************************************/
__attribute__((warn_unused_result))
static struct uds_record_name *generateRandomBlockNames(struct index_geometry *g)
{
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(vdo_allocate(g->records_per_chapter,
                                  "record names for chapter test",
                                  &names));
  unsigned int i;
  for (i = 0; i < g->records_per_chapter; i++) {
    createRandomBlockName(&names[i]);
  }
  return names;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static unsigned int generatePageNumber(struct index_geometry *g, unsigned int index)
{
  return index % g->record_pages_per_chapter;
}

/**********************************************************************/
__attribute__((warn_unused_result)) static
struct open_chapter_index *fillOpenChapter(struct uds_record_name *names,
                                           struct index_geometry *g,
                                           bool overflowFlag)
{
  struct open_chapter_index *oci;
  int overflowCount = 0;
  struct delta_index_stats stats;

  UDS_ASSERT_SUCCESS(uds_make_open_chapter_index(&oci, g, volumeNonce));
  uds_empty_open_chapter_index(oci, SAMPLE_CHAPTER_NUMBER);
  unsigned int i;
  for (i = 0; i < g->records_per_chapter; i++) {
    uds_get_delta_index_stats(&oci->delta_index, &stats);
    CU_ASSERT_EQUAL(stats.record_count + overflowCount, i);
    int result = uds_put_open_chapter_index_record(oci, &names[i], generatePageNumber(g, i));
    if (overflowFlag && (result == UDS_OVERFLOW)) {
      overflowCount++;
    } else {
      UDS_ASSERT_SUCCESS(result);
    }
  }

  uds_get_delta_index_stats(&oci->delta_index, &stats);
  CU_ASSERT_EQUAL(stats.record_count + overflowCount, g->records_per_chapter);
  return oci;
}

/**********************************************************************/
__attribute__((warn_unused_result))
static u8 *packOpenChapter(struct open_chapter_index *oci,
                           struct index_geometry *g, unsigned int numPages,
                           bool lastPage)
{
  u8 *indexPages;
  UDS_ASSERT_SUCCESS(vdo_allocate(numPages * g->bytes_per_page, "memory pages", &indexPages));
  u8 *pageOffset = indexPages;
  unsigned int firstList = 0;
  unsigned int page;
  for (page = 0; page < numPages; page++) {
    unsigned int numLists;
    UDS_ASSERT_SUCCESS(uds_pack_open_chapter_index_page(oci, pageOffset, firstList,
                                                        lastPage, &numLists));
    firstList += numLists;
    pageOffset += g->bytes_per_page;
  }
  CU_ASSERT_EQUAL(firstList, g->delta_lists_per_chapter);
  return indexPages;
}

/**********************************************************************/
__attribute__((warn_unused_result)) static struct delta_index_page *
setupChapterIndexPages(struct index_geometry *g, u8 *indexPages,
                       unsigned int numPages)
{
  struct delta_index_page *cip;
  UDS_ASSERT_SUCCESS(vdo_allocate(numPages, "chapter index pages", &cip));
  unsigned int page;
  for (page = 0; page < numPages; page++) {
    u8 *indexPage = &indexPages[g->bytes_per_page * page];
    UDS_ASSERT_SUCCESS(uds_initialize_chapter_index_page(&cip[page], g, indexPage, volumeNonce));
  }
  return cip;
}

/**********************************************************************/
static void verifyChapterIndexPage(struct open_chapter_index *openChapterIndex,
                                   struct delta_index_page   *chapterIndexPage)
{
  int first = chapterIndexPage->lowest_list_number;
  int last  = chapterIndexPage->highest_list_number;
  CU_ASSERT_EQUAL(SAMPLE_CHAPTER_NUMBER,
                  chapterIndexPage->virtual_chapter_number);
  int listNumber;
  for (listNumber = first; listNumber <= last; listNumber++) {
    struct delta_index_entry entry, openEntry;
    UDS_ASSERT_SUCCESS(uds_start_delta_index_search(&openChapterIndex->delta_index,
                                                    listNumber, 0, &openEntry));
    UDS_ASSERT_SUCCESS(uds_start_delta_index_search(&chapterIndexPage->delta_index,
                                                    listNumber - first, 0,
                                                    &entry));
    for (;;) {
      UDS_ASSERT_SUCCESS(uds_next_delta_index_entry(&openEntry));
      UDS_ASSERT_SUCCESS(uds_next_delta_index_entry(&entry));
      CU_ASSERT_EQUAL(openEntry.key, entry.key);
      CU_ASSERT_EQUAL(openEntry.at_end, entry.at_end);
      CU_ASSERT_EQUAL(openEntry.is_collision, entry.is_collision);
      CU_ASSERT_EQUAL(openEntry.delta, entry.delta);
      if (entry.at_end) {
        break;
      }
      CU_ASSERT_EQUAL(openEntry.value_bits, entry.value_bits);
      CU_ASSERT_EQUAL(openEntry.entry_bits, entry.entry_bits);
    }
  }
}

/**********************************************************************/
static void emptyChapterTest(void)
{
  struct uds_configuration *config = makeDenseConfiguration(1);
  struct index_geometry *g = config->geometry;

  // Create an open chapter index that is empty (no blocknames in it)
  struct open_chapter_index *oci;
  struct delta_index_stats stats;
  UDS_ASSERT_SUCCESS(uds_make_open_chapter_index(&oci, g, volumeNonce));
  uds_empty_open_chapter_index(oci, 0);
  uds_get_delta_index_stats(&oci->delta_index, &stats);
  CU_ASSERT_EQUAL(stats.record_count, 0);

  // Pack the index pages - this will test pages with empty lists, and will
  // test pages that have no lists at all
  u8 *indexPages = packOpenChapter(oci, g, g->index_pages_per_chapter, false);
  struct delta_index_page *cip
    = setupChapterIndexPages(g, indexPages, g->index_pages_per_chapter);

  uds_free_open_chapter_index(oci);
  vdo_free(cip);
  vdo_free(indexPages);
  uds_free_configuration(config);
}

/**********************************************************************/
static void basicChapterTest(void)
{
  struct uds_configuration *config = makeDenseConfiguration(1);
  struct index_geometry *g = config->geometry;
  struct uds_record_name *names = generateRandomBlockNames(g);
  struct open_chapter_index *oci = fillOpenChapter(names, g, false);
  u8 *indexPages = packOpenChapter(oci, g, g->index_pages_per_chapter, false);
  struct delta_index_page *cip
    = setupChapterIndexPages(g, indexPages, g->index_pages_per_chapter);
  unsigned int page;
  for (page = 0; page < g->index_pages_per_chapter; page++) {
    verifyChapterIndexPage(oci, &cip[page]);
  }
  uds_free_open_chapter_index(oci);

  // Make sure that all the names in the open_chapter_index are in one of our
  // ChapterIndexPages
  unsigned int i;
  for (i = 0; i < g->records_per_chapter; i++) {
    u32 deltaListNumber = uds_hash_to_chapter_delta_list(&names[i], g);
    bool inChapter = false;
    for (page = 0; page < g->index_pages_per_chapter; page++) {
      if ((cip[page].lowest_list_number <= deltaListNumber)
          && (deltaListNumber <= cip[page].highest_list_number)) {
        u16 entry;
        UDS_ASSERT_SUCCESS(uds_search_chapter_index_page(&cip[page], g, &names[i], &entry));
        CU_ASSERT_EQUAL(entry, generatePageNumber(g, i));
        inChapter = true;
      }
    }
    CU_ASSERT_TRUE(inChapter);
  }

  vdo_free(cip);
  vdo_free(indexPages);
  vdo_free(names);
  uds_free_configuration(config);
}

/**********************************************************************/
static void listOverflowTest(void)
{
  struct uds_configuration *config
    = makeDenseConfiguration(UDS_MEMORY_CONFIG_256MB);
  struct index_geometry *g = config->geometry;
  struct uds_record_name *names = generateRandomBlockNames(g);

  // Force all the names onto the same chapter delta list.  We are testing that
  // the open_chapter_index can deal with too many block names on the same
  // delta list.
  unsigned int i;
  for (i = 0; i < g->records_per_chapter; i++) {
    set_chapter_delta_list_bits(&names[i], g, 0);
  }

  struct open_chapter_index *oci = fillOpenChapter(names, g, true);
  u8 *indexPages = packOpenChapter(oci, g, g->index_pages_per_chapter, false);
  struct delta_index_page *cip
    = setupChapterIndexPages(g, indexPages, g->index_pages_per_chapter);
  unsigned int page;
  for (page = 0; page < g->index_pages_per_chapter; page++) {
    verifyChapterIndexPage(oci, &cip[page]);
  }
  uds_free_open_chapter_index(oci);

  vdo_free(cip);
  vdo_free(indexPages);
  vdo_free(names);
  uds_free_configuration(config);
}

/**********************************************************************/
static void pageOverflowTest(void)
{
  struct uds_configuration *config = makeDenseConfiguration(1);
  struct index_geometry *g = config->geometry;
  struct uds_record_name *names = generateRandomBlockNames(g);
  struct open_chapter_index *oci = fillOpenChapter(names, g, false);

  // Pack the entire open_chapter_index into a single page.  It won't fit, but
  // we want to handle this gracefully.
  u8 *indexPages = packOpenChapter(oci, g, 1, true);
  struct delta_index_page *cip = setupChapterIndexPages(g, indexPages, 1);

  verifyChapterIndexPage(oci, cip);
  uds_free_open_chapter_index(oci);

  vdo_free(cip);
  vdo_free(indexPages);
  vdo_free(names);
  uds_free_configuration(config);
}

/**********************************************************************/
static void bigEndianTest(void)
{
  struct uds_configuration *config = makeDenseConfiguration(1);
  struct index_geometry *g = config->geometry;
  struct uds_record_name *names = generateRandomBlockNames(g);
  struct open_chapter_index *oci = fillOpenChapter(names, g, false);
  u8 *indexPages = packOpenChapter(oci, g, g->index_pages_per_chapter, false);

  // Change the index pages to have headers written in big endian byte order.
  // This makes them like pages written on big endian hosts on RHEL8.0.
  unsigned int page;
  for (page = 0; page < g->index_pages_per_chapter; page++) {
    u8 *indexPage = &indexPages[g->bytes_per_page * page];
    swap_delta_index_page_endianness(indexPage);
  }

  struct delta_index_page *cip
    = setupChapterIndexPages(g, indexPages, g->index_pages_per_chapter);
  for (page = 0; page < g->index_pages_per_chapter; page++) {
    verifyChapterIndexPage(oci, &cip[page]);
  }
  uds_free_open_chapter_index(oci);
  vdo_free(cip);
  vdo_free(indexPages);
  vdo_free(names);
  uds_free_configuration(config);
}

/**********************************************************************/

static const CU_TestInfo chapterIndexTests[] = {
  {"Empty chapter", emptyChapterTest },
  {"Basic chapter", basicChapterTest },
  {"List overflow", listOverflowTest },
  {"Page overflow", pageOverflowTest },
  {"Big endian"   , bigEndianTest    },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "ChapterIndex_t1",
  .tests = chapterIndexTests,
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
