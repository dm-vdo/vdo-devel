/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VOLUME_UTILS_H
#define VOLUME_UTILS_H

#include "volume.h"

/************************************************************************
 * Creates a page array for storing page data written to a volume, so it
 * can be compared against reads later on
 *
 * @param numPages the number of pages to create
 * @param pageSize the size of each page
 *
 * @return pointer to page array to store page data into
 */
byte **makePageArray(unsigned int numPages, size_t pageSize);

/************************************************************************
 * Free up a page array created by makePageArray
 *
 * @param pages the array to free
 * @param numPages the number of pages in the array
 *
 */
void freePageArray(byte **pages, unsigned int numPages);

/**
 * Create a default volume chapter, with valid index and record pages
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 * @param chapter  the chapter to write
 * @param pages    pointer to pages array to fill in while writing
 *                 used for testing
 */
void writeTestVolumeChapter(struct volume *volume, struct geometry *geometry,
                            unsigned int chapter, byte **pages);

/**
 * Create a default volume file, with valid index and record pages
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 * @param pages    pointer to pages array to fill in while writing
 */
void writeTestVolumeData(struct volume *volume, struct geometry *geometry,
                         byte **pages);

#endif /* VOLUME_UTILS_H */
