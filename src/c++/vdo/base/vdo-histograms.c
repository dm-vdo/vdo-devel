// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "memory-alloc.h"

#include "vdo-histograms.h"

#include "histogram.h"

/**
 * vdo_initialize_histograms() - Make the set of internal histograms for a vdo.
 * @histograms: The histograms to initialize.
 *
 * Since these are only used for internal testing, allocation errors constructing them will be
 * ignored.
 */
void vdo_initialize_histograms(struct vdo_histograms *histograms)
{
	/*
	 * The numeric argument to make_logarithmic_jiffies_histogram is the number of orders of
	 * magnitude in the histogram. The smallest bucket corresponds to 1 jiffy which is 1 msec.
	 * on RedHat or 4 msec. on non-RedHat. Therefore the largest bucket for 4 is 10 seconds,
	 * for 5 is 100 seconds, and for 6 is 1000 seconds. Using a value that is too large is not
	 * expensive.
	 */
	histograms->post_histogram =
		make_logarithmic_jiffies_histogram("dedupe_post",
						   "Dedupe Index Post", "operations",
						   "response time", 4);
	histograms->query_histogram =
		make_logarithmic_jiffies_histogram("dedupe_query",
						   "Dedupe Index Query", "operations",
						   "response time", 4);
	histograms->update_histogram =
		make_logarithmic_jiffies_histogram("dedupe_update",
						   "Dedupe Index Update", "operations",
						   "response time", 4);
	histograms->flush_histogram =
		make_logarithmic_jiffies_histogram("flush",
						   "Forward External Flush Request",
						   "flushes", "latency", 6);
	histograms->read_ack_histogram =
		make_logarithmic_jiffies_histogram("acknowledge_read",
						   "Acknowledge External Read Request",
						   "reads", "response time", 5);
	histograms->write_ack_histogram =
		make_logarithmic_jiffies_histogram("acknowledge_write",
						   "Acknowledge External Write Request",
						   "writes", "response time", 5);
	histograms->discard_ack_histogram =
		make_logarithmic_jiffies_histogram("acknowledge_discard",
						   "Acknowledge External Discard Request",
						   "discards", "response time", 5);
	histograms->read_bios_histogram =
		make_logarithmic_jiffies_histogram("bio_read", "Read I/O",
						   "reads", "I/O time", 5);
	histograms->read_queue_histogram =
		make_logarithmic_jiffies_histogram("read_queue", "Read Queue",
						   "reads", "queue time", 5);
	histograms->write_bios_histogram =
		make_logarithmic_jiffies_histogram("bio_write", "Write I/O",
						   "writes", "I/O time", 5);
	histograms->write_queue_histogram =
		make_logarithmic_jiffies_histogram("write_queue", "Write Queue",
						   "writes", "queue time", 5);
	histograms->start_request_histogram =
		make_logarithmic_jiffies_histogram("bio_start", "Start Request",
						   "requests", "delay time", 5);
}

void vdo_set_histogram_limit(struct vdo_histograms *histograms, char *name,
			     char *value, unsigned int length)
{
	if (histogram_is_named(histograms->discard_ack_histogram, name) == 0) {
		set_histogram_limit(histograms->discard_ack_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->flush_histogram, name) == 0) {
		set_histogram_limit(histograms->flush_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->post_histogram, name) == 0) {
		set_histogram_limit(histograms->post_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->query_histogram, name) == 0) {
		set_histogram_limit(histograms->query_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->read_ack_histogram, name) == 0) {
		set_histogram_limit(histograms->read_ack_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->read_bios_histogram, name) == 0) {
		set_histogram_limit(histograms->read_bios_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->read_queue_histogram, name) == 0) {
		set_histogram_limit(histograms->read_queue_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->start_request_histogram, name) == 0) {
		set_histogram_limit(histograms->start_request_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->update_histogram, name) == 0) {
		set_histogram_limit(histograms->update_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->write_ack_histogram, name) == 0) {
		set_histogram_limit(histograms->write_ack_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->write_bios_histogram, name) == 0) {
		set_histogram_limit(histograms->write_bios_histogram, value, length);
		return;
	}
	if (histogram_is_named(histograms->write_queue_histogram, name) == 0) {
		set_histogram_limit(histograms->write_queue_histogram, value, length);
		return;
	}
}

void vdo_write_histograms(struct vdo_histograms *histograms, char **buf,
			  unsigned int *maxlen)
{
	write_sstring(NULL, "{ ", NULL, buf, maxlen);
	write_histogram(histograms->discard_ack_histogram, buf, maxlen);
	write_histogram(histograms->flush_histogram, buf, maxlen);
	write_histogram(histograms->post_histogram, buf, maxlen);
	write_histogram(histograms->query_histogram, buf, maxlen);
	write_histogram(histograms->read_ack_histogram, buf, maxlen);
	write_histogram(histograms->read_bios_histogram, buf, maxlen);
	write_histogram(histograms->read_queue_histogram, buf, maxlen);
	write_histogram(histograms->start_request_histogram, buf, maxlen);
	write_histogram(histograms->update_histogram, buf, maxlen);
	write_histogram(histograms->write_ack_histogram, buf, maxlen);
	write_histogram(histograms->write_bios_histogram, buf, maxlen);
	write_histogram(histograms->write_queue_histogram, buf, maxlen);
	write_sstring(NULL, "}", NULL, buf, maxlen);
}

/**
 * vdo_destroy_histograms() - Free the internal histograms of a vdo.
 * @histograms: The histograms to free.
 */
void vdo_destroy_histograms(struct vdo_histograms *histograms)
{
	free_histogram(vdo_forget(histograms->discard_ack_histogram));
	free_histogram(vdo_forget(histograms->flush_histogram));
	free_histogram(vdo_forget(histograms->post_histogram));
	free_histogram(vdo_forget(histograms->query_histogram));
	free_histogram(vdo_forget(histograms->read_ack_histogram));
	free_histogram(vdo_forget(histograms->read_bios_histogram));
	free_histogram(vdo_forget(histograms->read_queue_histogram));
	free_histogram(vdo_forget(histograms->start_request_histogram));
	free_histogram(vdo_forget(histograms->update_histogram));
	free_histogram(vdo_forget(histograms->write_ack_histogram));
	free_histogram(vdo_forget(histograms->write_bios_histogram));
	free_histogram(vdo_forget(histograms->write_queue_histogram));
}
