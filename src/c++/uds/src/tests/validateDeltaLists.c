// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "delta-index.h"
#include "testPrototypes.h"

/**********************************************************************/
void validateDeltaLists(const struct delta_memory *delta_memory)
{
  struct delta_list *delta_lists = delta_memory->delta_lists;
  unsigned int i;

  enum { GUARD_BITS = (sizeof(uint64_t) -1 ) * CHAR_BIT };

  /* Validate the delta index fields set by restoring a delta index. */

  /* There are not more collisions than total records. */
  CU_ASSERT_TRUE(delta_memory->collision_count <= delta_memory->record_count);

  /* Validate each delta list. */

  /* The head guard list starts at 0. */
  CU_ASSERT_TRUE(get_delta_list_start(&delta_lists[0]) == 0);

  /* The tail guard list ends at the end of the memory. */
  uint64_t num_bits
    = get_delta_list_end(&delta_lists[delta_memory->num_lists + 1]);
  CU_ASSERT_TRUE(num_bits == delta_memory->size * CHAR_BIT);

  /* The tail guard list contains sufficient guard bits. */
  int num_guard_bits
    = get_delta_list_size(&delta_lists[delta_memory->num_lists + 1]);

  CU_ASSERT_TRUE(num_guard_bits == GUARD_BITS);

  for (i = 0; i <= delta_memory->num_lists + 1; i++) {
    /* This list starts before it ends. */
    CU_ASSERT_TRUE(get_delta_list_start(&delta_lists[i])
                   <= get_delta_list_end(&delta_lists[i]));

    /* The rest of the checks do not apply to the tail guard list. */
    if (i > delta_memory->num_lists) {
      continue;
    }

    /* This list ends before the next one starts. */
    CU_ASSERT_TRUE(get_delta_list_end(&delta_lists[i])
                   <= get_delta_list_start(&delta_lists[i + 1]));

    /* The final check does not apply to the head guard list. */
    if (i == 0) {
      continue;
    }

    /* The saved offset is within the list. */
    CU_ASSERT_TRUE(delta_lists[i].save_offset
                   <= get_delta_list_size(&delta_lists[i]));
  }
}
