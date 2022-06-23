/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BLOCK_TEST_UTILS_H
#define BLOCK_TEST_UTILS_H

#include "uds.h"

/**
 * Get the number of blocks in a chapter
 *
 * @param session  The index session
 *
 * @return the number of block names that fit in a chapter
 **/
unsigned int getBlocksPerChapter(struct uds_index_session *session);

/**
 * Get the number of blocks in an index
 *
 * @param session  The index session
 *
 * @return the number of block names that fit in the index
 **/
unsigned long getBlocksPerIndex(struct uds_index_session *session);

/**
 * Get the number of chapters in an index
 *
 * @param session  The index session
 *
 * @return the number of chapters in the index
 **/
unsigned int getChaptersPerIndex(struct uds_index_session *session);

/**
 * Is the index sparse?
 *
 * @param session  The index session
 *
 * @return true if the index is sparse
 **/
bool isIndexSparse(struct uds_index_session *session);

#endif /* BLOCK_TEST_UTILS_H */
