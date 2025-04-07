/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_HISTOGRAMS_H
#define VDO_HISTOGRAMS_H

#include "histogram.h"

struct vdo_histograms {
	struct histogram *post_histogram;
	struct histogram *query_histogram;
	struct histogram *update_histogram;
	struct histogram *discard_ack_histogram;
	struct histogram *flush_histogram;
	struct histogram *read_ack_histogram;
	struct histogram *read_bios_histogram;
	struct histogram *read_queue_histogram;
	struct histogram *start_request_histogram;
	struct histogram *write_ack_histogram;
	struct histogram *write_bios_histogram;
	struct histogram *write_queue_histogram;
};

void vdo_initialize_histograms(struct vdo_histograms *histograms);

void vdo_set_histogram_limit(struct vdo_histograms *histograms, char *name, char *value,
			     unsigned int length);

void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen);

void vdo_destroy_histograms(struct vdo_histograms *histograms);

#endif /* VDO_HISTOGRAMS_H */
