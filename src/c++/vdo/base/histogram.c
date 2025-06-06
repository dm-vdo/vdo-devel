// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "memory-alloc.h"
#include "permassert.h"

#include "histogram.h"
#include "logger.h"

/*
 * Set NO_BUCKETS to streamline the histogram code by reducing it to tracking just minimum,
 * maximum, mean, etc. Only one bucket counter (the final one for "bigger" values) will be used, no
 * range checking is needed to find the right bucket, and no histogram will be reported. With newer
 * compilers, the histogram output code will be optimized out.
 */
enum {
#ifdef VDO_INTERNAL
	NO_BUCKETS = 0
#else
	NO_BUCKETS = 1
#endif
};

/*
 * Support histogramming in the VDO code.
 *
 * This is not a complete and general histogram package. It follows the XP practice of implementing
 * the "customer" requirements, and no more. We can support other requirements after we know what
 * they are.
 *
 * The code was originally borrowed from UDS, and includes both linear and logarithmic histograms.
 * VDO only uses the logarithmic histograms.
 *
 * All samples are u64 values.
 *
 * A unit conversion option is supported internally to allow sample values to be supplied in
 * "jiffies" and results to be reported via /sys in milliseconds. Depending on the system
 * configuration, this could mean a factor of four (a bucket for values of 1 jiffy is reported as
 * 4-7 milliseconds). In theory it could be a non-integer ratio (including less than one), but as
 * the x86-64 platforms we've encountered appear to use 1 or 4 milliseconds per jiffy, we don't
 * support non-integer values yet.
 *
 * All internal processing uses the values as passed to enter_histogram_sample. Conversions only
 * affect the values seen or input through the /sys interface, including possibly rounding a
 * "limit" value entered.
 */

struct histogram {
	/*
	 * These fields are ordered so that enter_histogram_sample touches only the first cache
	 * line.
	 */
	atomic64_t *counters; /* Counter for each bucket */
	u64 limit; /* We want to know how many samples are larger */
	atomic64_t sum; /* Sum of all the samples */
	atomic64_t count; /* Number of samples */
	atomic64_t minimum; /* Minimum value */
	atomic64_t maximum; /* Maximum value */
	atomic64_t unacceptable; /* Number of samples that exceed the limit */
	int num_buckets; /* The number of buckets */
	bool log_flag; /* True if the y scale should be logarithmic */
	/* These fields are used only when reporting results. */
	const char *name; /* Histogram name */
	const char *label; /* Histogram label */
	const char *counted_items; /* Name for things being counted */
	const char *metric; /* Term for value used to divide into buckets */
	const char *sample_units; /* Unit for measuring metric; NULL for count */
	unsigned int conversion_factor; /* Converts input units to reporting units */
};

/*
 * Fixed table defining the top value for each bucket of a logarithmic histogram. We arbitrarily
 * limit the histogram to 12 orders of magnitude.
 */
enum { MAX_LOG_SIZE = 12 };
static const u64 bottom_value[1 + 10 * MAX_LOG_SIZE] = {
	/* 0 to 10 - The first 10 buckets are linear */
	0,
	1,
	2,
	3,
	4,
	5,
	6,
	7,
	8,
	9,
	10,
	/*
	 * 10 to 100 - From this point on, the Nth entry of the table is
	 *             floor(exp10((double) N/10.0)).
	 */
	12,
	15,
	19,
	25,
	31,
	39,
	50,
	63,
	79,
	100,
	/* 100 to 1K */
	125,
	158,
	199,
	251,
	316,
	398,
	501,
	630,
	794,
	1000,
	/* 1K to 10K */
	1258,
	1584,
	1995,
	2511,
	3162,
	3981,
	5011,
	6309,
	7943,
	10000,
	/* 10K to 100K */
	12589,
	15848,
	19952,
	25118,
	31622,
	39810,
	50118,
	63095,
	79432,
	100000,
	/* 100K to 1M */
	125892,
	158489,
	199526,
	251188,
	316227,
	398107,
	501187,
	630957,
	794328,
	1000000,
	/* 1M to 10M */
	1258925,
	1584893,
	1995262,
	2511886,
	3162277,
	3981071,
	5011872,
	6309573,
	7943282,
	10000000,
	/* 10M to 100M */
	12589254,
	15848931,
	19952623,
	25118864,
	31622776,
	39810717,
	50118723,
	63095734,
	79432823,
	100000000,
	/* 100M to 1G */
	125892541,
	158489319,
	199526231,
	251188643,
	316227766,
	398107170,
	501187233,
	630957344,
	794328234,
	1000000000,
	/* 1G to 10G */
	1258925411L,
	1584893192L,
	1995262314L,
	2511886431L,
	3162277660L,
	3981071705L,
	5011872336L,
	6309573444L,
	7943282347L,
	10000000000L,
	/* 10G to 100G */
	12589254117L,
	15848931924L,
	19952623149L,
	25118864315L,
	31622776601L,
	39810717055L,
	50118723362L,
	63095734448L,
	79432823472L,
	100000000000L,
	/* 100G to 1T */
	125892541179L,
	158489319246L,
	199526231496L,
	251188643150L,
	316227766016L,
	398107170553L,
	501187233627L,
	630957344480L,
	794328234724L,
	1000000000000L,
};

static int max_bucket(struct histogram *h)
{
	int max = h->num_buckets;

	while ((max >= 0) && (atomic64_read(&h->counters[max]) == 0))
		max--;
	/* max == -1 means that there were no samples */
	return max;
}

static unsigned int divide_rounding_to_nearest(u64 number, u64 divisor)
{
	number += divisor / 2;
	return number / divisor;
}

void write_sstring(const char *prefix, char *value, char *suffix, char **buf,
		  unsigned int *maxlen)
{
	int count;

	count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
			  value, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_label(char *prefix, struct histogram *h, char *suffix,
				 char **buf, unsigned int *maxlen)
{
	int count;

	count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
			  h->label, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_counted_items(char *prefix, struct histogram *h,
					 char *suffix, char **buf, unsigned int *maxlen)
{
	int count;

	count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
			  h->counted_items, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_metric(char *prefix, struct histogram *h, char *suffix,
				  char **buf, unsigned int *maxlen)
{
	int count;

	count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
			  h->metric, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_unit(char *prefix, struct histogram *h, char *suffix,
				char **buf, unsigned int *maxlen)
{
	if (h->sample_units != NULL) {
		int count;

		count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
			  h->sample_units, suffix == NULL ? "" : suffix);
		*buf += count;
		*maxlen -= count;
	}
}

static void histogram_show_maximum(char *prefix, struct histogram *h, char *suffix,
				   char **buf, unsigned int *maxlen)
{
	int count;

	/* Maximum is initialized to 0. */
	unsigned long value = atomic64_read(&h->maximum);
	count = scnprintf(*buf, *maxlen, "%s%lu%s", prefix == NULL ? "" : prefix,
			  h->conversion_factor * value, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_minimum(char *prefix, struct histogram *h, char *suffix,
				   char **buf, unsigned int *maxlen)
{
	int count;

	/* Minimum is initialized to -1. */
	unsigned long value = ((atomic64_read(&h->count) > 0) ? atomic64_read(&h->minimum) : 0);
	count = scnprintf(*buf, *maxlen, "%s%lu%s", prefix == NULL ? "" : prefix,
			  h->conversion_factor * value, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_mean(char *prefix, struct histogram *h, char *suffix,
				char **buf, unsigned int *maxlen)
{
	int count;
	unsigned long sum_times1000_in_reporting_units;
	unsigned int mean_times1000;
	u64 value = atomic64_read(&h->count);

	if (value == 0) {
		count = scnprintf(*buf, *maxlen, "%s%s%s", prefix == NULL ? "" : prefix,
				  "0/0", suffix == NULL ? "" : suffix);
	} else {
		/* Compute mean, scaled up by 1000, in reporting units */
		sum_times1000_in_reporting_units = h->conversion_factor * atomic64_read(&h->sum) * 1000;
		mean_times1000 = divide_rounding_to_nearest(sum_times1000_in_reporting_units,
						    value);
		count = scnprintf(*buf, *maxlen, "%s%u.%03u%s", prefix == NULL ? "" : prefix,
				  mean_times1000 / 1000, mean_times1000 % 1000,
				  suffix == NULL ? "" : suffix);
	}
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_count(char *prefix, struct histogram *h, char *suffix,
				 char **buf, unsigned int *maxlen)
{
	int count;

	s64 value = atomic64_read(&h->count);
	count = scnprintf(*buf, *maxlen, "%s%lld%s", prefix == NULL ? "" : prefix,
			  value, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_histogram(char *prefix, struct histogram *h, char *suffix,
				     char **buf, unsigned int *maxlen)
{
	ssize_t count = 0;
	int max = max_bucket(h);
	u64 total = 0;
	int i;

	/* If max is -1, we'll fall through to reporting the total of zero. */

	write_sstring(prefix, "{ ", NULL, buf, maxlen);

	for (i = 0; i <= max; i++)
		total += atomic64_read(&h->counters[i]);

	for (i = 0; i <= max; i++) {
		u64 value = atomic64_read(&h->counters[i]);

		if (h->log_flag) {
			if (i == h->num_buckets) {
				count = scnprintf(*buf, *maxlen, "%s", "Bigger");
			} else {
				unsigned int lower = h->conversion_factor * bottom_value[i];
				unsigned int upper = h->conversion_factor * bottom_value[i + 1] - 1;
				count = scnprintf(*buf, *maxlen, "%u - %u", lower, upper);
			}
		} else {
			if (i == h->num_buckets) {
				count = scnprintf(*buf, *maxlen, "%s", "Bigger");
			} else {
				count = scnprintf(*buf, *maxlen, "%d", i);
			}
		}
		*buf += count;
		*maxlen -= count;

		count = scnprintf(*buf, *maxlen, " : %llu, ", value);
		*buf += count;
		*maxlen -= count;
	}

	write_sstring(NULL, "}", suffix, buf, maxlen);
}

static void histogram_show_unacceptable(char *prefix, struct histogram *h, char *suffix,
					char **buf, unsigned int *maxlen)
{
	int count;

	s64 value = atomic64_read(&h->unacceptable);
	count = scnprintf(*buf, *maxlen, "%s%lld%s", prefix == NULL ? "" : prefix,
			  value, suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static void histogram_show_limit(char *prefix, struct histogram *h, char *suffix,
				 char **buf, unsigned int *maxlen)
{
	int count;

	count = scnprintf(*buf, *maxlen, "%s%u%s", prefix == NULL ? "" : prefix,
			  (unsigned int) (h->conversion_factor * h->limit),
			  suffix == NULL ? "" : suffix);
	*buf += count;
	*maxlen -= count;
}

static struct histogram *make_histogram(const char *name,
					const char *label, const char *counted_items,
					const char *metric, const char *sample_units,
					int num_buckets, unsigned long conversion_factor,
					bool log_flag)
{
	struct histogram *h;

	if (vdo_allocate(1, struct histogram, "histogram", &h) != VDO_SUCCESS)
		return NULL;

	if (NO_BUCKETS)
		num_buckets = 0;

	if (num_buckets <= 10)
		/*
		 * The first buckets in a "logarithmic" histogram are still linear, but the
		 * bucket-search mechanism is a wee bit slower than for linear, so change the type.
		 */
		log_flag = false;

	h->name = name;
	h->label = label;
	h->counted_items = counted_items;
	h->metric = metric;
	h->sample_units = sample_units;
	h->log_flag = log_flag;
	h->num_buckets = num_buckets;
	h->conversion_factor = conversion_factor;
	atomic64_set(&h->minimum, -1UL);

	if (vdo_allocate(h->num_buckets + 1, atomic64_t, "histogram counters",
			 &h->counters) != VDO_SUCCESS) {
		return NULL;
	}

	return h;
}

/**
 * make_linear_histogram() - Allocate and initialize a histogram that uses linearly sized buckets.
 * @name: The short name of the histogram. This label is used for the sysfs node.
 * @init_label: The label for the sampled data. This label is used when we plot the data.
 * @counted_items: A name (plural) for the things being counted.
 * @metric: The measure being used to divide samples into buckets.
 * @sample_units: The unit (plural) for the metric, or NULL if it's a simple counter.
 * @size: The number of buckets. There are buckets for every value from 0 up to size (but not
 *        including) size. There is an extra bucket for larger samples.
 *
 * The histogram label reported via /sys is constructed from several of the values passed here; it
 * will be something like "Init Label Histogram - number of counted_items grouped by metric
 * (sample_units)", e.g., "Flush Forwarding Histogram - number of flushes grouped by latency
 * (milliseconds)". Thus counted_items and sample_units should be plural.
 *
 * The sample_units string will also be reported separately via another /sys entry to aid in
 * programmatic processing of the results, so the strings used should be consistent (e.g., always
 * "milliseconds" and not "ms" for milliseconds).
 *
 * Return: The histogram.
 */
struct histogram *make_linear_histogram(const char *name,
					const char *init_label,
					const char *counted_items, const char *metric,
					const char *sample_units, int size)
{
	return make_histogram(name, init_label, counted_items, metric,
			      sample_units, size, 1, false);
}

/**
 * make_logarithmic_histogram_with_conversion_factor() - Intermediate routine for creating
 *                                                       logarithmic histograms.
 * @name: The short name of the histogram. This label is used for the sysfs node.
 * @init_label: The label for the sampled data. This label is used when we plot the data.
 * @counted_items: A name (plural) for the things being counted.
 * @metric: The measure being used to divide samples into buckets.
 * @sample_units: The units (plural) for the metric, or NULL if it's a simple counter.
 * @log_size: The number of buckets. There are buckets for a range of sizes up to 10^log_size, and
 *            an extra bucket for larger samples.
 * @conversion_factor: Unit conversion factor for reporting.
 *
 * Limits the histogram size, and computes the bucket count from the orders-of-magnitude count.
 *
 * Return: The histogram.
 */
static struct histogram *
make_logarithmic_histogram_with_conversion_factor(const char *name,
						  const char *init_label, const char *counted_items,
						  const char *metric, const char *sample_units,
						  int log_size, u64 conversion_factor)
{
	if (log_size > MAX_LOG_SIZE)
		log_size = MAX_LOG_SIZE;
	return make_histogram(name, init_label, counted_items, metric,
			      sample_units, 10 * log_size, conversion_factor, true);
}

/**
 * make_logarithmic_histogram() - Allocate and initialize a histogram that uses logarithmically
 *                                sized buckets.
 * @name: The short name of the histogram. This label is used for the sysfs node.
 * @init_label: The label for the sampled data. This label is used when we plot the data.
 * @counted_items: A name (plural) for the things being counted.
 * @metric: The measure being used to divide samples into buckets.
 * @sample_units: The unit (plural) for the metric, or NULL if it's a simple counter.
 * @log_size: The number of buckets. There are buckets for a range of sizes up to 10^log_size, and
 *            an extra bucket for larger samples.
 *
 * Return: The histogram.
 */
struct histogram *make_logarithmic_histogram(const char *name,
					     const char *init_label,
					     const char *counted_items,
					     const char *metric,
					     const char *sample_units, int log_size)
{
	return make_logarithmic_histogram_with_conversion_factor(name,
								 init_label,
								 counted_items, metric,
								 sample_units, log_size, 1);
}

/**
 * make_logarithmic_jiffies_histogram() - Allocate and initialize a histogram that uses
 *                                        logarithmically sized buckets.
 * @name: The short name of the histogram. This label is used for the sysfs node.
 * @init_label: The label for the sampled data. This label is used when we plot the data.
 * @counted_items: A name (plural) for the things being counted.
 * @metric: The measure being used to divide samples into buckets.
 * @log_size: The number of buckets. There are buckets for a range of sizes up to 10^log_size, and
 *            an extra bucket for larger samples.
 *
 * Values are entered that count in jiffies, and they are reported in milliseconds.
 *
 * Return: The histogram.
 */
struct histogram *make_logarithmic_jiffies_histogram(const char *name,
						     const char *init_label,
						     const char *counted_items,
						     const char *metric, int log_size)
{
	/*
	 * If these fail, we have a jiffy duration that is not an integral number of milliseconds,
	 * and the unit conversion code needs updating.
	 */
	BUILD_BUG_ON(HZ > MSEC_PER_SEC);
	BUILD_BUG_ON((MSEC_PER_SEC % HZ) != 0);
	return make_logarithmic_histogram_with_conversion_factor(name,
								 init_label,
								 counted_items, metric,
								 "milliseconds",
								 log_size,
								 jiffies_to_msecs(1));
}

/**
 * enter_histogram_sample() - Enter a sample into a histogram.
 * @h: The histogram (may be NULL).
 * @sample: The sample.
 */
void enter_histogram_sample(struct histogram *h, u64 sample)
{
	u64 old_minimum, old_maximum;
	int bucket;

	if (h == NULL)
		return;

	if (h->log_flag) {
		int lo = 0;
		int hi = h->num_buckets;

		while (lo < hi) {
			int middle = (lo + hi) / 2;

			if (sample < bottom_value[middle + 1])
				hi = middle;
			else
				lo = middle + 1;
		}
		bucket = lo;
	} else {
		bucket = sample < h->num_buckets ? sample : h->num_buckets;
	}
	atomic64_inc(&h->counters[bucket]);
	atomic64_inc(&h->count);
	atomic64_add(sample, &h->sum);
	if ((h->limit > 0) && (sample > h->limit))
		atomic64_inc(&h->unacceptable);

	/*
	 * Theoretically this could loop a lot; in practice it should rarely do more than a single
	 * read, with no memory barrier, from a cache line we've already referenced above.
	 */
	old_maximum = atomic64_read(&h->maximum);

	while (old_maximum < sample) {
		u64 read_value = atomic64_cmpxchg(&h->maximum, old_maximum, sample);

		if (read_value == old_maximum)
			break;
		old_maximum = read_value;
	}

	old_minimum = atomic64_read(&h->minimum);

	while (old_minimum > sample) {
		u64 read_value = atomic64_cmpxchg(&h->minimum, old_minimum, sample);

		if (read_value == old_minimum)
			break;
		old_minimum = read_value;
	}
}

/**
 * write_histogram() - Writes histogram info into a bufer.
 * @histogram: The histogram to write.
 * @buf: The buffer to write into
 * @maxlen: The max size of the buffer
 */
void write_histogram(struct histogram *histogram, char **buf, unsigned int *maxlen)
{
	write_sstring(histogram->name, ": { ", NULL, buf, maxlen);
	histogram_show_label("label : ", histogram, ", ", buf, maxlen);
	histogram_show_counted_items("type : ", histogram, ", ", buf, maxlen);
	histogram_show_metric("metric : ", histogram, ", ", buf, maxlen);
	histogram_show_unit("unit : ", histogram, ", ", buf, maxlen);
	histogram_show_count("count : ", histogram, ", ", buf, maxlen);
	histogram_show_maximum("max : ", histogram, ", ", buf, maxlen);
	histogram_show_mean("mean : ", histogram, ", ", buf, maxlen);
	histogram_show_minimum("min : ", histogram, ", ", buf, maxlen);
	histogram_show_histogram("buckets : ", histogram, ", ", buf, maxlen);
	histogram_show_unacceptable("unacceptable : ", histogram, ", ", buf, maxlen);
	histogram_show_limit("limit : ", histogram, ", ", buf, maxlen);
	write_sstring(NULL, "}, ", NULL, buf, maxlen);
}

ssize_t set_histogram_limit(struct histogram *h, const char *buf, ssize_t length)
{
	unsigned int value;

	if ((length > 12) || (sscanf(buf, "%u", &value) != 1))
		return -EINVAL;
	/*
	 * Convert input from reporting units (e.g., milliseconds) to internal recording units
	 * (e.g., jiffies).
	 */
	h->limit = DIV_ROUND_UP(value, h->conversion_factor);
	atomic64_set(&h->unacceptable, 0);
	return length;
}

/**
 * free_histogram() - Free a histogram.
 * @histogram: The histogram to free.
 */
void free_histogram(struct histogram *histogram)
{
	vdo_free(histogram->counters);
	vdo_free(histogram);
}
