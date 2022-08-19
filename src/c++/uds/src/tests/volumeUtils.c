// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/dm-bufio.h>

#include "assertions.h"
#include "memory-alloc.h"
#include "random.h"
#include "volumeUtils.h"

/************************************************************************
 * Creates a page array for storing page data written to a volume, so it
 * can be compared against reads later on
 *
 * @param numPages the number of pages to create
 * @param pageSize the size of each page
 *
 * @return pointer to page array to store page data into
 */
byte **makePageArray(unsigned int numPages, size_t pageSize)
{
  byte **pages;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numPages, byte *, __func__, &pages));
  unsigned int i;
  for (i = 0; i < numPages; ++i) {
    UDS_ASSERT_SUCCESS(UDS_ALLOCATE(pageSize, byte, __func__, &pages[i]));
  }
  return pages;
}

/************************************************************************
 * Free up a page array created by makePageArray
 *
 * @param pages the array to free
 * @param numPages the number of pages in the array
 *
 */
void freePageArray(byte **pages, unsigned int numPages)
{
  if (pages != NULL) {
    unsigned int i;
    for (i = 0; i < numPages; ++i) {
      UDS_FREE(pages[i]);
    }
    UDS_FREE(pages);
  }
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
                            struct uds_chunk_record *records,
                            struct geometry *geometry)
{
  struct delta_index_stats stats;
  unsigned int i;

  for (i = 0; i < geometry->records_per_chapter; i++) {
    get_delta_index_stats(&oci->delta_index, &stats);
    CU_ASSERT_EQUAL(stats.record_count, i);
    int result = put_open_chapter_index_record(oci, &records[i].name,
                                               i / geometry->records_per_page);
    if (result != UDS_OVERFLOW) {
      UDS_ASSERT_SUCCESS(result);
    }
  }

  get_delta_index_stats(&oci->delta_index, &stats);
  CU_ASSERT_EQUAL(stats.record_count, geometry->records_per_chapter);
}

/**********************************************************************
 * Writes a single valid chapter to a volume file for testing
 *
 * @param volume    the volume to write to
 * @param geometry  the geometry to use
 * @param chapter   the chapter to write
 * @param pages     pointer to an array of pages that will contain a copy of
 *                  the page data that is written to the disk
 */
void writeTestVolumeChapter(struct volume *volume, struct geometry *geometry,
                            unsigned int chapter, byte **pages) {
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
  empty_open_chapter_index(chapterIndex, chapter);

  // Fill the delta list open chapter
  fillOpenChapter(chapterIndex, records, geometry);

  // Determine the position of the chapter in the volume file.
  int physicalPage = map_to_physical_page(geometry, chapter, 0);

  // Pack and write the delta chapter index pages to the volume.
  UDS_ASSERT_SUCCESS(write_index_pages(volume, physicalPage, chapterIndex,
                                       pages));

  // Sort and write the record pages to the volume.
  UDS_ASSERT_SUCCESS(write_record_pages(volume, physicalPage, records,
                                        &pages[geometry->index_pages_per_chapter]));

  UDS_ASSERT_SUCCESS(dm_bufio_write_dirty_buffers(volume->client));
  free_open_chapter_index(chapterIndex);
  UDS_FREE(records);
}

/**********************************************************************
 * Writes a valid volume for testing
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 * @param pages    pointer to an array of pages that will contain a copy of
 *                 the page data that is written to the disk
 */
void writeTestVolumeData(struct volume *volume, struct geometry *geometry,
                         byte **pages)
{
  unsigned int i;
  for (i = 0; i < geometry->chapters_per_volume; ++i) {
    writeTestVolumeChapter(volume, geometry, i,
                           &pages[i * geometry->pages_per_chapter]);
  }
}
