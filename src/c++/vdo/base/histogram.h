/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <linux/types.h>

struct histogram *make_linear_histogram(struct kobject *parent,
					const char *name,
					const char *init_label,
					const char *counted_items,
					const char *metric,
					const char *sample_units,
					int size);

struct histogram *make_logarithmic_histogram(struct kobject *parent,
					     const char *name,
					     const char *init_label,
					     const char *counted_items,
					     const char *metric,
					     const char *sample_units,
					     int log_size);

struct histogram *make_logarithmic_jiffies_histogram(struct kobject *parent,
						     const char *name,
						     const char *init_label,
						     const char *counted_items,
						     const char *metric,
						     int log_size);

void enter_histogram_sample(struct histogram *h, uint64_t sample);

void free_histogram(struct histogram *histogram);

#endif /* HISTOGRAM_H */
