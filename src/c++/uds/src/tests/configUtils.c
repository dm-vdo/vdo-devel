// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "testPrototypes.h"

#include "assertions.h"
#include "config.h"
#include "geometry.h"

/**********************************************************************/
void resizeDenseConfiguration(struct configuration *config,
                              size_t bytes_per_page,
                              unsigned int record_pages_per_chapter,
                              unsigned int chapters_per_volume)
{
  resizeSparseConfiguration(config, bytes_per_page, record_pages_per_chapter,
                            chapters_per_volume, 0, 0);
}

/**********************************************************************/
void resizeSparseConfiguration(struct configuration *config,
                               size_t bytes_per_page,
                               unsigned int record_pages_per_chapter,
                               unsigned int chapters_per_volume,
                               unsigned int sparse_chapters_per_volume,
                               unsigned int sparse_sample_rate)
{
  struct geometry *oldGeometry = config->geometry;
  if (bytes_per_page == 0) {
    bytes_per_page = oldGeometry->bytes_per_page;
  }
  if (record_pages_per_chapter == 0) {
    record_pages_per_chapter = oldGeometry->record_pages_per_chapter;
  }
  if (chapters_per_volume == 0) {
    chapters_per_volume = oldGeometry->chapters_per_volume;
  }
  if (sparse_chapters_per_volume == 0) {
    sparse_chapters_per_volume = oldGeometry->sparse_chapters_per_volume;
  }
  uds_free_geometry(oldGeometry);

  UDS_ASSERT_SUCCESS(uds_make_geometry(bytes_per_page,
                                       record_pages_per_chapter,
                                       chapters_per_volume,
                                       sparse_chapters_per_volume,
                                       0, 0, &config->geometry));
  if (sparse_sample_rate > 0) {
    config->sparse_sample_rate = sparse_sample_rate;
  }
}
