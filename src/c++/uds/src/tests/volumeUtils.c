// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/dm-bufio.h>
#include <linux/random.h>

#include "volumeUtils.h"

#include "assertions.h"
#include "memory-alloc.h"

/************************************************************************
 * Creates a page array for storing page data written to a volume, so it
 * can be compared against reads later on
 *
 * @param numPages the number of pages to create
 * @param pageSize the size of each page
 */
void makePageArray(unsigned int numPages, size_t pageSize)
{
  test_page_count = HEADER_PAGES_PER_VOLUME + numPages;
  UDS_ASSERT_SUCCESS(uds_allocate(test_page_count, u8 *, __func__, &test_pages));
  unsigned int i;
  for (i = 0; i < test_page_count; ++i) {
    UDS_ASSERT_SUCCESS(uds_allocate(pageSize, u8, __func__, &test_pages[i]));
  }
}

/************************************************************************
 * Free up a page array created by makePageArray
 */
void freePageArray(void)
{
  if (test_pages != NULL) {
    unsigned int i;
    for (i = 0; i < test_page_count; ++i) {
      uds_free(test_pages[i]);
    }
    uds_free(test_pages);
  }

  test_pages = NULL;
  test_page_count = 0;
}

/**********************************************************************
 * This method fills an open chapter index structure with a series of
 * record hashes.
 *
 * @param oci           The open chapter index to fill
 * @param records       The series of records to fill with
 * @param geometry      The geometry to use
 */
static void fillOpenChapter(struct open_chapter_index *oci,
                            struct uds_volume_record *records,
                            struct geometry *geometry)
{
  struct delta_index_stats stats;
  unsigned int i;

  for (i = 0; i < geometry->records_per_chapter; i++) {
    uds_get_delta_index_stats(&oci->delta_index, &stats);
    CU_ASSERT_EQUAL(stats.record_count, i);
    int result = uds_put_open_chapter_index_record(oci, &records[i].name,
                                                   i / geometry->records_per_page);
    if (result != UDS_OVERFLOW) {
      UDS_ASSERT_SUCCESS(result);
    }
  }

  uds_get_delta_index_stats(&oci->delta_index, &stats);
  CU_ASSERT_EQUAL(stats.record_count, geometry->records_per_chapter);
}

/**********************************************************************
 * Writes a single valid chapter to a volume file for testing
 *
 * @param volume    the volume to write to
 * @param geometry  the geometry to use
 * @param chapter   the chapter to write
 */
void writeTestVolumeChapter(struct volume *volume, struct geometry *geometry, u32 chapter)
{
  struct uds_volume_record *records;
  UDS_ASSERT_SUCCESS(uds_allocate(1 + geometry->records_per_chapter, struct uds_volume_record,
                                  __func__, &records));
  get_random_bytes((u8 *) records, BYTES_PER_RECORD * (1 + geometry->records_per_chapter));

  // Construct an empty delta chapter index for chapter zero. The chapter
  // write code doesn't really care if it's populated or not.
  struct open_chapter_index *chapterIndex;
  UDS_ASSERT_SUCCESS(uds_make_open_chapter_index(&chapterIndex, geometry, volume->nonce));
  CU_ASSERT_PTR_NOT_NULL(chapterIndex);
  uds_empty_open_chapter_index(chapterIndex, chapter);

  // Fill the delta list open chapter
  fillOpenChapter(chapterIndex, records, geometry);

  UDS_ASSERT_SUCCESS(uds_write_chapter(volume, chapterIndex, records));

  uds_free_open_chapter_index(chapterIndex);
  uds_free(records);
}
    
/**********************************************************************
 * Writes a valid volume for testing
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 */
void writeTestVolumeData(struct volume *volume, struct geometry *geometry)
{
  unsigned int i;
  for (i = 0; i < geometry->chapters_per_volume; ++i) {
    writeTestVolumeChapter(volume, geometry, i);
  }
}
