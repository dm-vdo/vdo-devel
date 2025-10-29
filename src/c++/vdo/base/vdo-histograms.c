// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "memory-alloc.h"

#include "vdo-histograms.h"

#include "histogram.h"

/*
 * The numeric argument to make_logarithmic_jiffies_histogram is the number of orders of
 * magnitude in the histogram. The smallest bucket corresponds to 1 jiffy which is 1 msec.
 * on RedHat or 4 msec. on non-RedHat. Therefore the largest bucket for 4 is 10 seconds,
 * for 5 is 100 seconds, and for 6 is 1000 seconds. Using a value that is too large is not
 * expensive.
 */
const struct histogram_info histogram_list[] = {
	[HISTOGRAM_DEDUPE_POST] = { "dedupe_post", "Dedupe Index Post", "operations", "response time", "milliseconds", 4 },
	[HISTOGRAM_DEDUPE_QUERY] = { "dedupe_query", "Dedupe Index Query", "operations", "response time", "milliseconds", 4 },
	[HISTOGRAM_DEDUPE_UPDATE] = { "dedupe_update", "Dedupe Index Update", "operations", "response time", "milliseconds", 4 },
	[HISTOGRAM_FLUSH] = { "flush", "Forward External Flush Request", "flushes", "latency", "milliseconds", 6 },
	[HISTOGRAM_ACKNOWLEDGE_READ] = { "acknowledge_read", "Acknowledge External Read Request", "reads", "response time", "milliseconds", 5 },
	[HISTOGRAM_ACKNOWLEDGE_WRITE] = { "acknowledge_write", "Acknowledge External Write Request", "writes", "response time", "milliseconds", 5 },
	[HISTOGRAM_ACKNOWLEDGE_DISCARD] = { "acknowledge_discard", "Acknowledge External Discard", "discards", "response time", "milliseconds", 5 },
	[HISTOGRAM_BIO_READ] = { "bio_read", "Read I/O", "reads", "I/O time", "milliseconds", 5 },
	[HISTOGRAM_READ_QUEUE] = { "read_queue", "Read Queue", "reads", "queue time", "milliseconds", 5 },
	[HISTOGRAM_BIO_WRITE] = { "bio_write", "Write I/O", "writes", "I/O time", "milliseconds", 5 },
	[HISTOGRAM_WRITE_QUEUE] = { "write_queue", "Write Queue", "writes", "queue time", "milliseconds", 5 },
	[HISTOGRAM_BIO_START] = { "bio_start", "Start Request", "requests", "delay time", "milliseconds", 5 },
};

/**
 * vdo_initialize_histograms() - Make the set of internal histograms for a vdo.
 * @histograms: The histograms to initialize.
 *
 * Since these are only used for internal testing, allocation errors
 * constructing them will be ignored.
 */
void vdo_initialize_histograms(struct vdo_histograms *histograms)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		histograms->histograms[i] = make_logarithmic_jiffies_histogram(&histogram_list[i]);
	}
	histograms->post_histogram = histograms->histograms[HISTOGRAM_DEDUPE_POST];
	histograms->query_histogram = histograms->histograms[HISTOGRAM_DEDUPE_QUERY];
	histograms->update_histogram = histograms->histograms[HISTOGRAM_DEDUPE_UPDATE];
	histograms->flush_histogram = histograms->histograms[HISTOGRAM_FLUSH];
	histograms->read_ack_histogram = histograms->histograms[HISTOGRAM_ACKNOWLEDGE_READ];
	histograms->write_ack_histogram = histograms->histograms[HISTOGRAM_ACKNOWLEDGE_WRITE];
	histograms->discard_ack_histogram = histograms->histograms[HISTOGRAM_ACKNOWLEDGE_DISCARD];
	histograms->read_bios_histogram = histograms->histograms[HISTOGRAM_BIO_READ];
	histograms->read_queue_histogram = histograms->histograms[HISTOGRAM_READ_QUEUE];
	histograms->write_bios_histogram = histograms->histograms[HISTOGRAM_BIO_WRITE];
	histograms->write_queue_histogram = histograms->histograms[HISTOGRAM_WRITE_QUEUE];
	histograms->start_request_histogram = histograms->histograms[HISTOGRAM_BIO_START];
}

/**
 * vdo_store_histogram_limit() - Store a histogram limit.
 * @histograms: The list of histograms.
 * @name: The name of the histogram to store the limit in.
 * @value: The value to store in the histogram.
 * @length: The length of the value to store in the histogram.
 */
void vdo_store_histogram_limit(struct vdo_histograms *histograms, char *name, char *value,
			       unsigned int length)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		if (histograms->histograms[i] == NULL)
			continue;
		if (strcmp(histogram_list[i].name, name) == 0) {
			histogram_store_limit(histograms->histograms[i], value, length);
			return;
		}
	}
}

/**
 * vdo_write_histograms() - Write the histograms to a buffer in JSON format.
 * @histograms: The histograms to write.
 * @buf: The buffer to write to.
 * @maxlen: The maximum length of the buffer.
 */
void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen)
{
	histogram_write_string(buf, maxlen, "[ ");
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		if (histograms->histograms[i] == NULL)
			continue;
		write_histogram(histograms->histograms[i], buf, maxlen);
		if (i < HISTOGRAM_LAST - 1) {
			histogram_write_string(buf, maxlen, ", ");
		}
	}
	histogram_write_string(buf, maxlen, " ]");
}

/**
 * vdo_destroy_histograms() - Free the internal histograms of a vdo.
 * @histograms: The histograms to free.
 */
void vdo_destroy_histograms(struct vdo_histograms *histograms)
{
	for (int i = 0; i < HISTOGRAM_LAST; i++) {
		free_histogram(histograms->histograms[i]);
	}
}
