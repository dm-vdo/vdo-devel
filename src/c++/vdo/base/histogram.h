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

void __printf(3, 4) histogram_write_item(char **buffer, unsigned int *maxlen,
					 const char *format, ...);

static inline void histogram_write_string(char **buffer, unsigned int *maxlen, char *string)
{
	histogram_write_item(buffer, maxlen, "%s", string);
}

void write_histogram(struct histogram *histogram, char **buf, unsigned int *maxlen);

bool histogram_is_named(struct histogram *histogram, const char *name);

ssize_t histogram_store_limit(struct histogram *h, const char *buf, size_t length);

void free_histogram(struct histogram *histogram);

#endif /* HISTOGRAM_H */
