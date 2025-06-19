/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <linux/types.h>

struct histogram *make_linear_histogram(const char *name,
					const char *init_label,
					const char *counted_items, const char *metric,
					const char *sample_units, int size);

struct histogram *make_logarithmic_histogram(const char *name,
					     const char *init_label,
					     const char *counted_items,
					     const char *metric,
					     const char *sample_units, int log_size);

struct histogram *make_logarithmic_jiffies_histogram(const char *name,
						     const char *init_label,
						     const char *counted_items,
						     const char *metric, int log_size);

void enter_histogram_sample(struct histogram *h, u64 sample);

ssize_t set_histogram_limit(struct histogram *histogram, const char *buf, ssize_t length);

void write_sstring(const char *prefix, char *value, char *suffix, char **buf,
		   unsigned int *maxlen);

void write_histogram(struct histogram *histogram, char **buf, unsigned int *maxlen);

void free_histogram(struct histogram *histogram);

#endif /* HISTOGRAM_H */
