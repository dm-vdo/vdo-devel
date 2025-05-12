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
	struct histogram *histogram[HISTOGRAM_LAST];
};

void vdo_initialize_histograms(struct vdo_histograms *histograms);
void vdo_set_histogram_limit(struct vdo_histograms *histograms, char *name, char *value,
			     unsigned int length);
void vdo_enter_histogram_sample(struct vdo_histograms *histograms,
				enum histogram_types type, u64 sample);
void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen);
void vdo_destroy_histograms(struct vdo_histograms *histograms);

#endif /* VDO_HISTOGRAMS_H */
