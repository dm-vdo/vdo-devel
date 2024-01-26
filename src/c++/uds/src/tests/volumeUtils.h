/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
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
 */
void makePageArray(unsigned int numPages, size_t pageSize);

/************************************************************************
 * Free up a page array created by makePageArray
 */
void freePageArray(void);

/**
 * Create a default volume chapter, with valid index and record pages
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 * @param chapter  the chapter to write
 */
void writeTestVolumeChapter(struct volume *volume, struct index_geometry *geometry, u32 chapter);

/**
 * Create a default volume file, with valid index and record pages
 *
 * @param volume   the volume to write to
 * @param geometry the geometry to use
 */
void writeTestVolumeData(struct volume *volume, struct index_geometry *geometry);

#endif /* VOLUME_UTILS_H */
