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
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  UDS_ASSERT_SUCCESS(uds_make_index_layout(config, true, &layout));
  geometry = config->geometry;

  UDS_ASSERT_SUCCESS(uds_make_volume(config, layout, &volume));
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
  uds_free_volume(volume);
  uds_free_configuration(config);
  uds_free_index_layout(UDS_FORGET(layout));
}

/**********************************************************************/
static void testWriteChapter(void)
{
  uint64_t chapterNumber = 0;
  uds_forget_chapter(volume, chapterNumber);

  unsigned int zoneCount = config->zone_count;
  struct open_chapter_zone **chapters;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(zoneCount, struct open_chapter_zone *,
                                  "open chapters", &chapters));
  unsigned int i;
  for (i = 0; i < zoneCount; i++) {
    UDS_ASSERT_SUCCESS(uds_make_open_chapter(geometry, zoneCount, &chapters[i]));
  }

  struct uds_record_name *hashes;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(geometry->records_per_chapter,
                                  struct uds_record_name, "names", &hashes));
  struct uds_record_data *metadata;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(geometry->records_per_chapter,
                                  struct uds_record_data, "records",
                                  &metadata));

  // Thanks to zoning, the chapter on disk might not be completely full.
  unsigned int highestRecord = 0;
  unsigned int zone;
  for (zone = 0; zone < zoneCount; ++zone) {
    for (i = zone; ; i += zoneCount) {
      createRandomBlockName(&hashes[i]);
      createRandomMetadata(&metadata[i]);

      unsigned int remaining;
      remaining = uds_put_open_chapter(chapters[zone], &hashes[i], &metadata[i]);
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
    = (sizeof(struct uds_volume_record)
       * (1 + volume->geometry->records_per_chapter));
  struct uds_volume_record *collatedRecords;
  UDS_ASSERT_SUCCESS(uds_allocate_cache_aligned(collatedRecordsSize,
                                                "collated records",
                                                &collatedRecords));
  struct open_chapter_index *openChapterIndex;
  UDS_ASSERT_SUCCESS(uds_make_open_chapter_index(&openChapterIndex,
                                                 volume->geometry,
                                                 volume->nonce));
  uds_empty_open_chapter_index(openChapterIndex, 0);
  UDS_ASSERT_SUCCESS(uds_close_open_chapter(chapters,
                                            zoneCount,
                                            volume,
                                            openChapterIndex,
                                            collatedRecords,
                                            chapterNumber));
  uds_free_open_chapter_index(openChapterIndex);
  UDS_FREE(collatedRecords);

  for (zone = 0; zone < zoneCount; ++zone) {
    uds_free_open_chapter(chapters[zone]);
  }

  // Test reading records directly from the record pages.
  unsigned int physicalChapterNumber
    = uds_map_to_physical_chapter(geometry, chapterNumber);
  unsigned int recordNumber = 0;
  unsigned int j, page;
  for (page = 0; page < geometry->record_pages_per_chapter; ++page) {
    unsigned int pageNumber = page + geometry->index_pages_per_chapter;
    u8 *pageData;
    // Make sure the page read is synchronous
    UDS_ASSERT_SUCCESS(uds_get_volume_record_page(volume,
                                                  physicalChapterNumber,
                                                  pageNumber,
                                                  &pageData));

    for (j = 0; j < geometry->records_per_page; ++j) {
      struct uds_record_data retMetadata;
      bool found = search_record_page(pageData, &hashes[recordNumber],
                                      geometry, &retMetadata);
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
    bool found;
    struct uds_request request = {
      .record_name = hashes[i],
      .virtual_chapter = chapterNumber,
      .unbatched = true,
    };

    UDS_ASSERT_SUCCESS(uds_search_volume_page_cache(volume, &request, &found));
    CU_ASSERT_TRUE(found);
    UDS_ASSERT_BLOCKDATA_EQUAL(&request.old_metadata, &metadata[i]);
  }
  UDS_FREE(metadata);
  UDS_FREE(hashes);
  UDS_FREE(chapters);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"WriteChapter", testWriteChapter},
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
