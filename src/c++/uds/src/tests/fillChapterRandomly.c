// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"
#include "testRequests.h"

/**********************************************************************/
void fillChapterRandomly(struct uds_index *index)
{
  uint64_t chapterToFill = index->zones[0]->newest_virtual_chapter; 

  while (index->zones[0]->newest_virtual_chapter == chapterToFill) {
    struct uds_request request = { .type = UDS_POST };
    createRandomBlockNameInZone(index, 0, &request.record_name);
    createRandomMetadata(&request.new_metadata);
    verify_test_request(index, &request, false, NULL);
  }
  wait_for_idle_index(index);
}
