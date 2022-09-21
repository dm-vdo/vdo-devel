// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "hash-utils.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"

static struct configuration *config;
static struct io_factory    *factory;
static struct geometry      *geometry;
static uint64_t              vcn;
static unsigned int         *listNumbers;

/**********************************************************************/
static void setup(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .name = getTestIndexName(),
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));

  geometry = config->geometry;
  vcn = geometry->chapters_per_volume * 3;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE((geometry->index_pages_per_chapter
                                   * geometry->chapters_per_volume),
                                  unsigned int, __func__, &listNumbers));
}

/**********************************************************************/
static void cleanup(void)
{
  put_uds_io_factory(factory);
  free_configuration(config);
  UDS_FREE(listNumbers);
}

/**********************************************************************/
static void fillChapter(struct index_page_map *map,
                        struct geometry       *geometry,
                        uint64_t               vcn,
                        unsigned int           chapterNumber,
                        unsigned int          *listNumbers)
{
  unsigned int lastIndexPageNumber = geometry->index_pages_per_chapter - 1;
  unsigned int mean
    = geometry->delta_lists_per_chapter / geometry->index_pages_per_chapter;

  unsigned int listNumber, page;
  for (page = 0, listNumber = 0; page < lastIndexPageNumber; page++) {
    listNumber += mean + (random() % ((mean / 5) + 1)) - (mean / 10);
    if (listNumber >= geometry->delta_lists_per_chapter) {
      listNumber = geometry->delta_lists_per_chapter - 1;
    }

    if (listNumbers != NULL) {
      listNumbers[page] = listNumber;
    }

    update_index_page_map(map, vcn, chapterNumber, page, listNumber);
  }

  unsigned int lastDeltaListNumber = geometry->delta_lists_per_chapter - 1;
  if (listNumbers != NULL) {
    listNumbers[lastIndexPageNumber] = lastDeltaListNumber;
  }

  update_index_page_map(map, vcn, chapterNumber, lastIndexPageNumber,
                        lastDeltaListNumber);
}

/**********************************************************************/
static void verifyChapter(struct index_page_map *map,
                          struct geometry       *geometry,
                          unsigned int           chapter,
                          unsigned int          *listNumbers)
{
  unsigned int firstList = 0;
  unsigned int list, page;
  for (page = 0; page < geometry->index_pages_per_chapter; page++) {
    for (list = firstList; list <= listNumbers[page]; list++) {
      // Put the list number into a record name so it maps to the list number.
      struct uds_record_name name;
      memset(&name, 0, sizeof(name));
      set_chapter_delta_list_bits(&name, geometry, list);
      CU_ASSERT_EQUAL(list, hash_to_chapter_delta_list(&name, geometry));
      CU_ASSERT_EQUAL(page, find_index_page_number(map, &name, chapter));
    }
    firstList = listNumbers[page] + 1;
  }
}

/**********************************************************************/
static void testDefault(void)
{
  struct index_page_map *map;
  UDS_ASSERT_SUCCESS(make_index_page_map(geometry, &map));

  unsigned int chapter = 12;
  fillChapter(map, geometry, 0, chapter - 1, NULL);
  fillChapter(map, geometry, 0, chapter, listNumbers);
  fillChapter(map, geometry, 0, chapter + 1, NULL);

  verifyChapter(map, geometry, chapter, listNumbers);

  free_index_page_map(map);
}

/**********************************************************************/
static void testReadWrite(void)
{
  // Write an index page map
  struct index_page_map *map;
  UDS_ASSERT_SUCCESS(make_index_page_map(geometry, &map));

  unsigned int chap;
  for (chap = 0; chap < geometry->chapters_per_volume; ++chap) {
    fillChapter(map, geometry, vcn + chap, chap,
                &listNumbers[chap * geometry->index_pages_per_chapter]);
  }
  CU_ASSERT_EQUAL(map->last_update, vcn + geometry->chapters_per_volume - 1);

  uint64_t mapBlocks
    = DIV_ROUND_UP(compute_index_page_map_save_size(geometry), UDS_BLOCK_SIZE);

  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(make_buffered_writer(factory, 0, mapBlocks, &writer));
  UDS_ASSERT_SUCCESS(write_index_page_map(map, writer));
  free_buffered_writer(writer);
  free_index_page_map(UDS_FORGET(map));

  // Read and verify the index page map
  UDS_ASSERT_SUCCESS(make_index_page_map(geometry, &map));

  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(make_buffered_reader(factory, 0, mapBlocks, &reader));
  UDS_ASSERT_SUCCESS(read_index_page_map(map, reader));

  CU_ASSERT_EQUAL(map->last_update, vcn + geometry->chapters_per_volume - 1);

  for (chap = 0; chap < geometry->chapters_per_volume; ++chap) {
    verifyChapter(map, geometry, chap,
                  &listNumbers[chap * geometry->index_pages_per_chapter]);
  }

  free_buffered_reader(reader);
  free_index_page_map(map);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Default",     testDefault     },
  { "ReadWrite",   testReadWrite   },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "IndexPageMap_t1",
  .initializer = setup,
  .cleaner     = cleanup,
  .tests       = tests,
};

// Entry point required by the module loader. Return a pointer to the
// const CU_SuiteInfo structure.

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
