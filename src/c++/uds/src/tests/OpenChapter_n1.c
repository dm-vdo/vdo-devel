// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "open-chapter.h"
#include "testPrototypes.h"
#include "volume.h"

enum { CHAPTER_COUNT = 32 };
enum { NAMES_PER_CHAPTER = 256 * 1024 };

static ktime_t  totalOpenTime    = 0;
static ktime_t  totalCloseTime   = 0;
static ktime_t  totalPutTime     = 0;
static uint64_t totalRecordCount = 0;

/**********************************************************************/
static void reportZoneTime(uint64_t records, ktime_t openTime,
                           ktime_t putTime)
{
  char *openString, *putString, *putPerRecord;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&openString, openTime));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&putString, putTime));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&putPerRecord, putTime / records));
  albPrint("reset_open_chapter:  %s", openString);
  albPrint("put_open_chapter:   %s (%s per record) for %llu records",
           putString, putPerRecord, (unsigned long long) records);
  uds_free(openString);
  uds_free(putString);
  uds_free(putPerRecord);
}

/**********************************************************************/
static void reportCloseTime(uint64_t records, ktime_t closeTime)
{
  char *closeString, *closePerRecord;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&closeString, closeTime));
  UDS_ASSERT_SUCCESS(rel_time_to_string(&closePerRecord, closeTime / records));
  albPrint("closeOpenChapter: %s (%s per record)", closeString, closePerRecord);
  uds_free(closeString);
  uds_free(closePerRecord);
}

/**********************************************************************/
static void fillOpenChapterZone(struct open_chapter_zone *openChapter)
{
  // We do not want to time the generation of the random names
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(uds_allocate(openChapter->capacity,
                                  struct uds_record_name,
                                  "record names for chapter test",
                                  &names));
  unsigned int i;
  for (i = 0; i < openChapter->capacity; i++) {
    createRandomBlockName(&names[i]);
  }

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  uds_reset_open_chapter(openChapter);
  ktime_t openTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);

  start = current_time_ns(CLOCK_MONOTONIC);
  uint64_t recordCount = 0;
  unsigned int remaining;
  for (remaining = UINT_MAX; remaining > 0;) {
    struct uds_record_data metaData;

    remaining = uds_put_open_chapter(openChapter, &names[recordCount], &metaData);
    ++recordCount;
  }
  ktime_t putTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);

  CU_ASSERT_TRUE(recordCount == openChapter->capacity);
  reportZoneTime(recordCount, openTime, putTime);

  totalOpenTime += openTime;
  totalPutTime  += putTime;
  uds_free(names);
}

/**********************************************************************/
static void fillOpenChapter(struct open_chapter_zone **openChapters,
                            struct volume *volume, unsigned int zoneCount,
                            int chapterNumber)
{
  uint64_t recordCount = 0;
  unsigned int zone;
  for (zone = 0; zone < zoneCount; zone++) {
    fillOpenChapterZone(openChapters[zone]);
    recordCount += openChapters[zone]->size;
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

  ktime_t start = current_time_ns(CLOCK_MONOTONIC);
  UDS_ASSERT_SUCCESS(uds_close_open_chapter(openChapters,
                                            zoneCount,
                                            volume,
                                            openChapterIndex,
                                            collatedRecords,
                                            chapterNumber));
  ktime_t closeTime = ktime_sub(current_time_ns(CLOCK_MONOTONIC), start);
  reportCloseTime(recordCount, closeTime);

  uds_free_open_chapter_index(openChapterIndex);
  uds_free(collatedRecords);

  totalRecordCount += recordCount;
  totalCloseTime   += closeTime;
}

/**********************************************************************/
static void testFilling(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
    .bdev = getTestBlockDevice(),
  };
  struct uds_configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, 0, 0, CHAPTER_COUNT);
  struct index_layout *layout;
  UDS_ASSERT_SUCCESS(uds_make_index_layout(config, true, &layout));

  struct volume *volume;
  UDS_ASSERT_SUCCESS(uds_make_volume(config, layout, &volume));

  unsigned int zoneCount = config->zone_count;
  struct open_chapter_zone **openChapters;
  UDS_ASSERT_SUCCESS(uds_allocate(zoneCount, struct open_chapter_zone *,
                                  "open chapters", &openChapters));
  unsigned int i;
  for (i = 0; i < zoneCount; i++) {
    UDS_ASSERT_SUCCESS(uds_make_open_chapter(volume->geometry, zoneCount, &openChapters[i]));
  }

  for (i = 0; i < CHAPTER_COUNT; i++) {
    fillOpenChapter(openChapters, volume, zoneCount, i);
  }

  reportZoneTime(totalRecordCount, totalOpenTime, totalPutTime);
  reportCloseTime(totalRecordCount, totalCloseTime);

  for (i = 0; i < zoneCount; i++) {
    uds_free_open_chapter(openChapters[i]);
  }
  uds_free(openChapters);
  uds_free_volume(volume);
  uds_free_configuration(config);
  uds_free_index_layout(layout);
  putTestBlockDevice(params.bdev);
}

/**********************************************************************/
static const CU_TestInfo openChapterPerformanceTests[] = {
  {"Open Chapter Put performance", testFilling },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "OpenChapter_n1",
  .tests = openChapterPerformanceTests,
};

/**
 * Entry point required by the module loader. Return a pointer to the
 * const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
