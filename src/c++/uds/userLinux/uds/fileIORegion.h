/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef FILE_IO_REGION_H
#define FILE_IO_REGION_H

#include "io-factory.h"
#include "ioRegion.h"
#include "fileUtils.h"

/**
 * Make an IO region using an open file descriptor.
 *
 * @param [in]  factory    The IO factory holding the open file descriptor.
 * @param [in]  fd         The file descriptor.
 * @param [in]  access     The access kind for the file.
 * @param [in]  offset     The byte offset to the start of the region.
 * @param [in]  size       Size of the file region (in bytes).
 * @param [out] region_ptr The new region.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check make_file_region(struct io_factory *factory,
				  int fd,
				  enum file_access access,
				  off_t offset,
				  size_t size,
				  struct io_region **region_ptr);

#endif // FILE_IO_REGION_H
