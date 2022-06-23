// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
void cbStatus(enum uds_request_type type __attribute__((unused)),
              int status,
              OldCookie cookie __attribute__((unused)),
              struct uds_chunk_data *duplicateAddress __attribute__((unused)),
              struct uds_chunk_data *canonicalAddress __attribute__((unused)),
              struct uds_chunk_name *blockName __attribute__((unused)),
              void *data __attribute__((unused)))
{
  UDS_ASSERT_SUCCESS(status);
}
