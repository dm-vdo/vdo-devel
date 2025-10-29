/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_HISTOGRAMS_H
#define VDO_HISTOGRAMS_H

#include "histogram.h"

enum histogram_types {
	HISTOGRAM_DEDUPE_POST,
	HISTOGRAM_DEDUPE_QUERY,
	HISTOGRAM_DEDUPE_UPDATE,
	HISTOGRAM_FLUSH,
	HISTOGRAM_ACKNOWLEDGE_READ,
	HISTOGRAM_ACKNOWLEDGE_WRITE,
	HISTOGRAM_ACKNOWLEDGE_DISCARD,
	HISTOGRAM_BIO_READ,
	HISTOGRAM_READ_QUEUE,
	HISTOGRAM_BIO_WRITE,
	HISTOGRAM_WRITE_QUEUE,
	HISTOGRAM_BIO_START,
	HISTOGRAM_LAST,
};

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
	struct histogram *histograms[HISTOGRAM_LAST];
};

void vdo_initialize_histograms(struct vdo_histograms *histograms);

void vdo_store_histogram_limit(struct vdo_histograms *histograms, char *name,
			       char *value, unsigned int length);

void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen);

void vdo_destroy_histograms(struct vdo_histograms *histograms);

#endif /* VDO_HISTOGRAMS_H */
