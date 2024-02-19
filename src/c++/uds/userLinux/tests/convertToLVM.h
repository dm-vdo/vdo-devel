/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef CONVERT_TO_LVM_H
#define CONVERT_TO_LVM_H

#include "indexer.h"

/**
 * Shrinks a UDS index to give VDO space to allow for LVM metadata to
 * be prefixed while retaining as much deduplication as possible. This
 * is done by reducing the chapter count by one and moving the super
 * block and the configuration block to the end of the vacated space,
 * thereby freeing space equal to the size of a chapter at the
 * beginning of the index.
 * 
 * If the operation is successful, the struct uds_parameters pointed
 * to by the parameters argument will have been modified to represent
 * the new memory size and the number of bytes in a chapter will be
 * returned in the location pointed to by the chapter_size argument.
 *
 * @param parameters    The parameters of the index
 * @param freed_space   The minimum amount of space to free at the start
 *                      of the device, in bytes. Must be a multiple of 4K.      
 * @param chapter_size  A place to return the size in bytes of the
 *                      chapter that was eliminated
 * @return  UDS_SUCCESS or an error code
 */
int __must_check uds_convert_to_lvm(struct uds_parameters *parameters,
				    size_t freed_space,
				    off_t *chapter_size);

#endif /* CONVERT_TO_LVM_H */
