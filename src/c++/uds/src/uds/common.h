/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef COMMON_H
#define COMMON_H

#include "string-utils.h"
#include "uds.h"

enum {
	KILOBYTE = 1024,
	MEGABYTE = KILOBYTE * KILOBYTE,
	GIGABYTE = KILOBYTE * MEGABYTE
};

struct uds_volume_record {
	struct uds_record_name name;
	struct uds_record_data data;
};

#endif /* COMMON_H */
