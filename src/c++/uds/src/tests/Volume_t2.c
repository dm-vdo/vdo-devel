// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/dm-bufio.h>

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "memory-alloc.h"
#include "open-chapter.h"
#include "testPrototypes.h"
#include "volume.h"
#include "volumeUtils.h"

static struct geometry      *geometry;
static struct configuration *config;
static struct index_layout  *layout;
static struct volume        *volume;

/**********************************************************************/
static void init(uds_memory_config_size_t memGB)
{
  struct uds_parameters params = {
    .memory_size = memGB,
    .name = getTestIndexName(),
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  UDS_ASSERT_SUCCESS(make_uds_index_layout(config, true, &layout));
  geometry = config->geometry;

  UDS_ASSERT_SUCCESS(make_volume(config, layout, &volume));
}

/**********************************************************************/
static void initDefault(void)
{
  init(1);
}

/**********************************************************************/
static void initSmall(void)
{
  init(UDS_MEMORY_CONFIG_256MB);
}

/**********************************************************************/
static void deinit(void)
{
  free_volume(volume);
  free_configuration(config);
  free_uds_index_layout(UDS_FORGET(layout));
}

/**********************************************************************/
static void writeAndVerifyPage(unsigned int chapter, unsigned int page)
{
  byte **pages
    = makePageArray(geometry->pages_per_chapter, geometry->bytes_per_page);
  unsigned int physPage = 1 + (chapter * geometry->pages_per_chapter) + page;

  writeTestVolumeChapter(volume, geometry, chapter, pages);

  struct dm_buffer *volume_page;
  UDS_ASSERT_KERNEL_SUCCESS(dm_bufio_read(volume->client, physPage,
                                          &volume_page));
  UDS_ASSERT_EQUAL_BYTES(dm_bufio_get_block_data(volume_page), pages[page],
                         geometry->bytes_per_page);
  dm_bufio_release(volume_page);

  byte *pageData;
  // Make sure the page read is synchronous
  UDS_ASSERT_SUCCESS(get_volume_page(volume, chapter, page, &pageData, NULL));
  UDS_ASSERT_EQUAL_BYTES(pageData, pages[page], geometry->bytes_per_page);

  freePageArray(pages, geometry->pages_per_chapter);
}

/**********************************************************************/
static void writeAndVerifyChapterIndex(unsigned int chapter)
{
  unsigned int i;
  byte **pages
    = makePageArray(geometry->pages_per_chapter, geometry->bytes_per_page);
  writeTestVolumeChapter(volume, geometry, chapter, pages);

  struct dm_buffer *volume_page;
  int physicalPage = map_to_physical_page(geometry, chapter, 0);
  for (i = 0; i < geometry->index_pages_per_chapter; i++) {
    UDS_ASSERT_KERNEL_SUCCESS(dm_bufio_read(volume->client, physicalPage + i,
                                            &volume_page));
    UDS_ASSERT_EQUAL_BYTES(dm_bufio_get_block_data(volume_page), pages[i],
                           geometry->bytes_per_page);
    dm_bufio_release(volume_page);
  }

  freePageArray(pages, geometry->pages_per_chapter);
  // cppcheck-suppress resourceLeak
}

/**********************************************************************/
static void testGetChapterIndex(void)
{
  // Write to chapter zero
  writeAndVerifyChapterIndex(0);

  // write to last chapter
  writeAndVerifyChapterIndex(geometry->chapters_per_volume - 1);
}

/**********************************************************************/
static void testGetPage(void)
{
  unsigned int firstPage = 0;
  unsigned int lastPage = geometry->pages_per_chapter - 1;

  // write data to chapter zero, page zero
  writeAndVerifyPage(0, firstPage);
  // write data to chapter zero, page N
  writeAndVerifyPage(0, lastPage);
  // write data to chapter N, page zero
  unsigned int lastChapter = geometry->chapters_per_volume - 1;
  writeAndVerifyPage(lastChapter, firstPage);
  // write data to chapter N, page N
  writeAndVerifyPage(lastChapter, lastPage);
}

/**********************************************************************/
static void testWriteChapter(void)
{
  uint64_t chapterNumber = 0;
  UDS_ASSERT_SUCCESS(forget_chapter(volume, chapterNumber));

  unsigned int zoneCount = config->zone_count;
  struct open_chapter_zone **chapters;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(zoneCount, struct open_chapter_zone *,
                                  "open chapters", &chapters));
  unsigned int i;
  for (i = 0; i < zoneCount; i++) {
    UDS_ASSERT_SUCCESS(make_open_chapter(geometry, zoneCount, &chapters[i]));
  }

  struct uds_chunk_name *hashes;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(geometry->records_per_chapter,
                                  struct uds_chunk_name, "names", &hashes));
  struct uds_chunk_data *metadata;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(geometry->records_per_chapter,
                                  struct uds_chunk_data, "records",
                                  &metadata));

  // Thanks to zoning, the chapter on disk might not be completely full.
  unsigned int highestRecord = 0;
  unsigned int zone;
  for (zone = 0; zone < zoneCount; ++zone) {
    for (i = zone; ; i += zoneCount) {
      createRandomBlockName(&hashes[i]);
      createRandomMetadata(&metadata[i]);

      unsigned int remaining;
      remaining = put_open_chapter(chapters[zone], &hashes[i], &metadata[i]);
      if (remaining == 0) {
        if (i > highestRecord) {
          highestRecord = i;
        }
        break;
      }
      CU_ASSERT_TRUE(i < geometry->records_per_chapter);
    }
  }

  size_t collatedRecordsSize
    = (sizeof(struct uds_chunk_record)
        * (1 + volume->geometry->records_per_chapter));
  struct uds_chunk_record *collatedRecords;
  UDS_ASSERT_SUCCESS(uds_allocate_cache_aligned(collatedRecordsSize,
                                                "collated records",
                                                &collatedRecords));
  struct open_chapter_index *openChapterIndex;
  UDS_ASSERT_SUCCESS(make_open_chapter_index(&openChapterIndex,
  					     volume->geometry,
                                             volume->nonce));
  empty_open_chapter_index(openChapterIndex, 0);
  UDS_ASSERT_SUCCESS(close_open_chapter(chapters, zoneCount, volume,
                                        openChapterIndex, collatedRecords,
                                        chapterNumber));
  free_open_chapter_index(openChapterIndex);
  UDS_FREE(collatedRecords);

  for (zone = 0; zone < zoneCount; ++zone) {
    free_open_chapter(chapters[zone]);
  }

  // Test reading records directly from the record pages.
  unsigned int physicalChapterNumber
    = map_to_physical_chapter(geometry, chapterNumber);
  unsigned int recordNumber = 0;
  unsigned int j, page;
  for (page = 0; page < geometry->record_pages_per_chapter; ++page) {
    byte *pageData;
    // Make sure the page read is synchronous
    UDS_ASSERT_SUCCESS(get_volume_page(volume, physicalChapterNumber,
                                       page
                                         + geometry->index_pages_per_chapter,
                                       &pageData, NULL));

    for (j = 0; j < geometry->records_per_page; ++j) {
      struct uds_chunk_data retMetadata;
      bool found = search_record_page(pageData,
                                    &hashes[recordNumber], geometry,
                                    &retMetadata);
      CU_ASSERT_TRUE(found);
      UDS_ASSERT_BLOCKDATA_EQUAL(&retMetadata, &metadata[recordNumber]);
      ++recordNumber;
      if (recordNumber > highestRecord) {
        break;
      }
    }
  }

  // Test reading records through the index pages.
  for (i = 0; i < highestRecord; ++i) {
    struct uds_chunk_data retMetadata;
    bool found;

    UDS_ASSERT_SUCCESS(search_volume_page_cache(volume, NULL, &hashes[i],
                                                chapterNumber, &retMetadata,
                                                &found));
    CU_ASSERT_TRUE(found);
    UDS_ASSERT_BLOCKDATA_EQUAL(&retMetadata, &metadata[i]);
  }
  UDS_FREE(metadata);
  UDS_FREE(hashes);
  UDS_FREE(chapters);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"GetPage",            testGetPage},
  {"WriteChapter",       testWriteChapter},
  {"GetChapterIndex",    testGetChapterIndex},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suites[] = {
  {
    .name        = "Volume_t2.normal",
    .initializer = initDefault,
    .cleaner     = deinit,
    .tests       = tests,
    .next        = &suites[1],
  },
  {
    .name        = "Volume_t2.small",
    .initializer = initSmall,
    .cleaner     = deinit,
    .tests       = tests,
  }
};

const CU_SuiteInfo *initializeModule(void)
{
  return suites;
}
