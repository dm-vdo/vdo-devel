/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
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

void vdo_initialize_histograms(struct kobject *parent,
			       struct vdo_histograms *histograms);

void vdo_destroy_histograms(struct vdo_histograms *histograms);

#endif /* VDO_HISTOGRAMS_H */
