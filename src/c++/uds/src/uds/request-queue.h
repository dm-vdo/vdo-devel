/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "compiler.h"
#include "type-defs.h"
#include "uds.h"

struct uds_request_queue;

typedef void uds_request_queue_processor_t(struct uds_request *);

int __must_check
make_uds_request_queue(const char *queue_name,
		       uds_request_queue_processor_t *processor,
		       struct uds_request_queue **queue_ptr);

void uds_request_queue_enqueue(struct uds_request_queue *queue,
			       struct uds_request *request);

void uds_request_queue_finish(struct uds_request_queue *queue);

#endif /* REQUEST_QUEUE_H */
