// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "index.h"
#include "config.h"
#include "random.h"
#include "testPrototypes.h"

/**********************************************************************/
void createRandomBlockNameInZone(const struct uds_index *index,
                                 unsigned int            zone,
                                 struct uds_chunk_name  *name)
{
  unsigned int nameZone = MAX_ZONES;
  while (nameZone != zone) {
    createRandomBlockName(name);
    nameZone = get_volume_index_zone(index->volume_index, name);
  }
}
