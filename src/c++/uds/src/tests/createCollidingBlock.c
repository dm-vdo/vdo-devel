// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "hash-utils.h"
#include "testPrototypes.h"

/**********************************************************************/
void createCollidingBlock(const struct uds_record_name *orig,
                          struct uds_record_name *collision)
{
  createRandomBlockName(collision);
  uint64_t addrField = uds_extract_volume_index_bytes(orig);
  set_volume_index_bytes(collision, addrField);
}
