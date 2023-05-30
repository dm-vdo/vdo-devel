// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/bits.h>

#include "assertions.h"
#include "delta-index.h"
#include "testPrototypes.h"

/**********************************************************************/
void validateDeltaLists(const struct delta_zone *delta_zone)
{
  struct delta_list *delta_lists = delta_zone->delta_lists;
  unsigned int i;

  enum { GUARD_BITS = (sizeof(uint64_t) -1 ) * BITS_PER_BYTE };

  /* Validate the delta index fields set by restoring a delta index. */

  /* There are not more collisions than total records. */
  CU_ASSERT_TRUE(delta_zone->collision_count <= delta_zone->record_count);

  /* Validate each delta list. */

  /* The head guard list starts at 0. */
  CU_ASSERT_TRUE(delta_lists[0].start == 0);

  /* The tail guard list ends at the end of the memory. */
  struct delta_list *tail_list = &delta_lists[delta_zone->list_count + 1];
  uint64_t num_bits = tail_list->start + tail_list->size;;
  CU_ASSERT_TRUE(num_bits == delta_zone->size * BITS_PER_BYTE);

  /* The tail guard list contains sufficient guard bits. */
  CU_ASSERT_TRUE(tail_list->size == GUARD_BITS);

  for (i = 0; i <= delta_zone->list_count + 1; i++) {
    /* This list starts before it ends. */
    CU_ASSERT_TRUE(delta_lists[i].start
                   <= delta_lists[i].start + delta_lists[i].size);

    /* The rest of the checks do not apply to the tail guard list. */
    if (i > delta_zone->list_count) {
      continue;
    }

    /* This list ends before the next one starts. */
    CU_ASSERT_TRUE(delta_lists[i].start + delta_lists[i].size
                   <= delta_lists[i + 1].start);

    /* The final check does not apply to the head guard list. */
    if (i == 0) {
      continue;
    }

    /* The saved offset is within the list. */
    CU_ASSERT_TRUE(delta_lists[i].save_offset <= delta_lists[i].size);
  }
}
