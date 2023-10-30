// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/kobject.h>

#include "memory-alloc.h"

#include "vdo-histograms.h"

#include "histogram.h"

/**
 * vdo_initialize_histograms() - Make the set of internal histograms for a vdo.
 * @parent: The parent kobject of the histograms.
 * @histograms: The histograms to initialize.
 *
 * Since these are only used for internal testing, allocation errors constructing them will be
 * ignored.
 */
void vdo_initialize_histograms(struct kobject *parent,
			       struct vdo_histograms *histograms)
{
	/*
	 * The numeric argument to make_logarithmic_jiffies_histogram is the number of orders of
	 * magnitude in the histogram. The smallest bucket corresponds to 1 jiffy which is 1 msec.
	 * on RedHat or 4 msec. on non-RedHat. Therefore the largest bucket for 4 is 10 seconds,
	 * for 5 is 100 seconds, and for 6 is 1000 seconds. Using a value that is too large is not
	 * expensive.
	 */
	histograms->post_histogram =
		make_logarithmic_jiffies_histogram(parent, "dedupe_post",
						   "Dedupe Index Post",
						   "operations",
						   "response time", 4);
	histograms->query_histogram =
		make_logarithmic_jiffies_histogram(parent, "dedupe_query",
						   "Dedupe Index Query",
						   "operations",
						   "response time", 4);
	histograms->update_histogram =
		make_logarithmic_jiffies_histogram(parent, "dedupe_update",
						   "Dedupe Index Update",
						   "operations",
						   "response time", 4);
	histograms->flush_histogram =
		make_logarithmic_jiffies_histogram(parent, "flush",
						   "Forward External Flush Request",
						   "flushes", "latency", 6);
	histograms->read_ack_histogram =
		make_logarithmic_jiffies_histogram(parent, "acknowledge_read",
						   "Acknowledge External Read Request",
						   "reads", "response time",
						   5);
	histograms->write_ack_histogram =
		make_logarithmic_jiffies_histogram(parent, "acknowledge_write",
						   "Acknowledge External Write Request",
						   "writes", "response time",
						   5);
	histograms->discard_ack_histogram =
		make_logarithmic_jiffies_histogram(parent,
						   "acknowledge_discard",
						   "Acknowledge External Discard Request",
						   "discards", "response time",
						   5);
	histograms->read_bios_histogram =
		make_logarithmic_jiffies_histogram(parent, "bio_read",
						   "Read I/O", "reads",
						   "I/O time", 5);
	histograms->read_queue_histogram =
		make_logarithmic_jiffies_histogram(parent, "read_queue",
						   "Read Queue", "reads",
						   "queue time", 5);
	histograms->write_bios_histogram =
		make_logarithmic_jiffies_histogram(parent, "bio_write",
						   "Write I/O", "writes",
						   "I/O time", 5);
	histograms->write_queue_histogram =
		make_logarithmic_jiffies_histogram(parent, "write_queue",
						   "Write Queue", "writes",
						   "queue time", 5);
	histograms->start_request_histogram =
		make_logarithmic_jiffies_histogram(parent, "bio_start",
						   "Start Request", "requests",
						   "delay time", 5);
}

/**
 * vdo_destroy_histograms() - Free the internal histograms of a vdo.
 * @histograms: The histograms to free.
 */
void vdo_destroy_histograms(struct vdo_histograms *histograms)
{
	free_histogram(UDS_FORGET(histograms->discard_ack_histogram));
	free_histogram(UDS_FORGET(histograms->flush_histogram));
	free_histogram(UDS_FORGET(histograms->post_histogram));
	free_histogram(UDS_FORGET(histograms->query_histogram));
	free_histogram(UDS_FORGET(histograms->read_ack_histogram));
	free_histogram(UDS_FORGET(histograms->read_bios_histogram));
	free_histogram(UDS_FORGET(histograms->read_queue_histogram));
	free_histogram(UDS_FORGET(histograms->start_request_histogram));
	free_histogram(UDS_FORGET(histograms->update_histogram));
	free_histogram(UDS_FORGET(histograms->write_ack_histogram));
	free_histogram(UDS_FORGET(histograms->write_bios_histogram));
	free_histogram(UDS_FORGET(histograms->write_queue_histogram));
}
