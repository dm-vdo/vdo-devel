// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
void cbStatus(enum uds_request_type type __attribute__((unused)),
              int status,
              OldCookie cookie __attribute__((unused)),
              struct uds_record_data *duplicateAddress __attribute__((unused)),
              struct uds_record_data *canonicalAddress __attribute__((unused)),
              struct uds_record_name *blockName __attribute__((unused)),
              void *data __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(status);
}
