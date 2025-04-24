// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "memory-alloc.h"

#include "vdo-histograms.h"

#include "histogram.h"

struct histogram_info {
	char *name;
	char *label;
	char *counted_items;
	char *metric;
	int log_size;
};

/*
 * The numeric argument to make_logarithmic_jiffies_histogram is the number of orders of
 * magnitude in the histogram. The smallest bucket corresponds to 1 jiffy which is 1 msec.
 * on RedHat or 4 msec. on non-RedHat. Therefore the largest bucket for 4 is 10 seconds,
 * for 5 is 100 seconds, and for 6 is 1000 seconds. Using a value that is too large is not
 * expensive.
 */
const struct histogram_info histogram_list[] = {
	[HISTOGRAM_DEDUPE_POST] = { "dedupe_post", "Dedupe Index Post", "operations", "response time", 4 },
	[HISTOGRAM_DEDUPE_QUERY] = { "dedupe_query", "Dedupe Index Query", "operations", "response time", 4 },
	[HISTOGRAM_DEDUPE_UPDATE] = { "dedupe_update", "Dedupe Index Update", "operations", "response time", 4 },
	[HISTOGRAM_FLUSH] = { "flush", "Forward External Flush Request", "flushes", "latency", 6 },
	[HISTOGRAM_ACKNOWLEDGE_READ] = { "acknowledge_read", "Acknowledge External Read Request", "reads", "response time", 5 },
	[HISTOGRAM_ACKNOWLEDGE_WRITE] = { "acknowledge_write", "Acknowledge External Write Request", "writes", "response time", 5 },
	[HISTOGRAM_ACKNOWLEDGE_DISCARD] = { "acknowledge_discard", "Acknowledge External Discard", "discards", "response time", 5 },
	[HISTOGRAM_BIO_READ] = { "bio_read", "Read I/O", "reads", "I/O time", 5 },
	[HISTOGRAM_READ_QUEUE] = { "read_queue", "Read Queue", "reads", "queue time", 5 },
	[HISTOGRAM_BIO_WRITE] = { "bio_write", "Write I/O", "writes", "I/O time", 5 },
	[HISTOGRAM_WRITE_QUEUE] = { "write_queue", "Write Queue", "writes", "queue time", 5 },
	[HISTOGRAM_BIO_START] = { "bio_start", "Start Request", "requests", "delay time", 5 },
};

/**
 * vdo_initialize_histograms() - Make the set of internal histograms for a vdo.
 * @histograms: The histograms to initialize.
 *
 * Since these are only used for internal testing, allocation errors constructing them will be
 * ignored.
 */
void vdo_initialize_histograms(struct vdo_histograms *histograms)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		struct histogram_info info = histogram_list[i];
		histograms->histogram[i] = make_logarithmic_jiffies_histogram(info.name, info.label, info.counted_items, info.metric, info.log_size);
	}
}

void vdo_set_histogram_limit(struct vdo_histograms *histograms, char *name,
			     char *value, unsigned int length)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		if (strcmp(histogram_list[i].name, name) == 0) {
			set_histogram_limit(histograms->histogram[i], value, length);
			return;
		}
	}
}

void vdo_enter_histogram_sample(struct vdo_histograms *histograms,
				enum histogram_types type, u64 sample)
{
	enter_histogram_sample(histograms->histogram[type], sample);
}

void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen)
{
	write_sstring(NULL, "{ ", NULL, buf, maxlen);
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		write_histogram(histograms->histogram[i], buf, maxlen);
	}
	write_sstring(NULL, "}", NULL, buf, maxlen);
}

/**
 * vdo_destroy_histograms() - Free the internal histograms of a vdo.
 * @histograms: The histograms to free.
 */
void vdo_destroy_histograms(struct vdo_histograms *histograms)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		free_histogram(histograms->histogram[i]);
	}
}
