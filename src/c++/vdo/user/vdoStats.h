/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef VDO_STATS_H
#define VDO_STATS_H

#include "types.h"

/**
 * Read vdo statistics from a buffer
 *
 * @param buf     pointer to the buffer
 * @param stats   pointer to the statistics
 *
 * @return VDO_SUCCESS or an error
 */
int read_vdo_stats(char *buf, struct vdo_statistics *stats);

/**
 * Write vdo statistics to stdout
 *
 * @param stats   pointer to the statistics
 *
 * @return VDO_SUCCESS or an error
 */
int vdo_write_stats(struct vdo_statistics *stats);

#endif  /* VDO_STATS_H */
