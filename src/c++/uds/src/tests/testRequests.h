/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#include "index.h"

void initialize_test_requests(void);

void uninitialize_test_requests(void);

/**
 * A wrapper for uds_enqueue_request() that dispatches a request to the index.
 *
 * @param index    The index
 * @param request  The request to dispatch
 **/
void submit_test_request(struct uds_index *index,
                         struct uds_request *request);

/**
 * A wrapper for submit_test_request() that dispatches a request and does
 * some verification of the response.
 *
 * @param index             The index
 * @param request           The request to dispatch
 * @param expectFound       Whether the chuck is expected to be in the index
 * @param expectedMetaData  The expected old chunk metadata, if found
 **/
void verify_test_request(struct uds_index *index,
                         struct uds_request *request,
                         bool expectFound,
                         const struct uds_record_data *expectedMetaData);
