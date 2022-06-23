/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef OPENCHAPTER_H
#define OPENCHAPTER_H 1

#include "common.h"
#include "geometry.h"
#include "index.h"

/**
 * open_chapter handles writing the open chapter records to the volume, and
 * also manages all the tools to generate and parse the open chapter file. The
 * open chapter file interleaves records from each open_chapter_zone structure.
 *
 * <p>Once each open chapter zone is filled, the records are interleaved to
 * preserve temporal locality, the index pages are generated through a
 * delta chapter index, and the record pages are derived by sorting each
 * page-sized batch of records by their names.
 *
 * <p>Upon index shutdown, the open chapter zone records are again
 * interleaved, and the records are stored as a single array. The hash
 * slots are not preserved, since the records may be reassigned to new
 * zones at load time.
 **/

/**
 * Close the open chapter and write it to disk.
 *
 * @param chapter_zones          The zones of the chapter to close
 * @param zone_count             The number of zones
 * @param volume                 The volume to which to write the chapter
 * @param chapter_index          The open_chapter_index to use while writing
 * @param collated_records       Collated records array to use while writing
 * @param virtual_chapter_number The virtual chapter number of the open chapter
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check close_open_chapter(struct open_chapter_zone **chapter_zones,
				    unsigned int zone_count,
				    struct volume *volume,
				    struct open_chapter_index *chapter_index,
				    struct uds_chunk_record *collated_records,
				    uint64_t virtual_chapter_number);

/**
 * Write out a partially filled chapter to a file.
 *
 * @param index        the index to save the data from
 * @param writer       the writer to write out the chapters
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check save_open_chapters(struct uds_index *index,
				    struct buffered_writer *writer);

/**
 * Read a partially filled chapter from a file.
 *
 * @param index        the index to load the data into
 * @param reader       the buffered reader to read from
 *
 * @return UDS_SUCCESS on success
 **/
int __must_check load_open_chapters(struct uds_index *index,
				    struct buffered_reader *reader);

/**
 * Compute the size of the maximum open chapter save image.
 *
 * @param geometry      the index geometry
 *
 * @return the number of bytes of the largest possible open chapter save
 *         image
 **/
uint64_t compute_saved_open_chapter_size(struct geometry *geometry);

#endif /* OPENCHAPTER_H */
