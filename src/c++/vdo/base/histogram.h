/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <linux/types.h>

struct histogram_info {
	char *name; /* The identifier name of the histogram */
	char *init_label; /* The label for the sampled data */
	char *counted_items; /* A name (plural) for the things being counted */
	char *metric; /* The measure being used to divide samples into buckets */
	char *sample_units; /* The unit (plural) for the metric, or NULL if it's counter */
	int log_size; /* The number of buckets */
};

struct histogram *make_linear_histogram(const struct histogram_info *info);
struct histogram *make_logarithmic_histogram(const struct histogram_info *info);
struct histogram *make_logarithmic_jiffies_histogram(const struct histogram_info *info);

void enter_histogram_sample(struct histogram *h, u64 sample);

void __printf(3, 4) histogram_write_item(char **buffer, unsigned int *maxlen,
					 const char *format, ...);

static inline void histogram_write_string(char **buffer, unsigned int *maxlen, char *string)
{
	histogram_write_item(buffer, maxlen, "%s", string);
}

void write_histogram(struct histogram *histogram, char **buf, unsigned int *maxlen);

ssize_t histogram_store_limit(struct histogram *h, const char *buf, size_t length);

void free_histogram(struct histogram *histogram);

#endif /* HISTOGRAM_H */
