/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef SLAB_SUMMARY_READER_H
#define SLAB_SUMMARY_READER_H

#include "slab-summary-format.h"
#include "types.h"

#include "userVDO.h"

/**
 * Read the contents of the slab summary into a single set of summary entries.
 *
 * @param [in]  vdo          The vdo from which to read the summary
 * @param [out] entries_ptr  A pointer to hold the loaded entries
 *
 * @return VDO_SUCCESS or an error code
 **/
int __must_check
readSlabSummary(UserVDO *vdo, struct slab_summary_entry **entriesPtr);

#endif // SLAB_SUMMARY_UTILS_H
