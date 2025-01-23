/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

/*
 * Implement a mapping from pointer to u32, (ab)using int_map.
 *
 * Without a pointer to return, we can't distinguish missing-key from
 * stored-zero indications. But our tests don't really care.
 */

#include "int-map.h"

struct ptr_u32_map;

static inline int __must_check vdo_ptr_u32_map_create(size_t initial_capacity,
                                                      struct ptr_u32_map **map_ptr)
{
  return vdo_int_map_create(initial_capacity, (struct int_map **)map_ptr);
}

static inline void vdo_ptr_u32_map_free(struct ptr_u32_map *map)
{
  vdo_int_map_free((struct int_map *)map);
}

static inline size_t vdo_ptr_u32_map_size(const struct ptr_u32_map *map)
{
  return vdo_int_map_size((const struct int_map *)map);
}

// N.B.: Not-found returns zero, indistiguishable from a stored zero.
static inline u32 vdo_ptr_u32_map_get(struct ptr_u32_map *map, void *key)
{
  void *pointer_value = vdo_int_map_get((struct int_map *)map, (unsigned long)key);
  return (u32)(unsigned long)pointer_value;
}

static inline int __must_check vdo_ptr_u32_map_put(struct ptr_u32_map *map, void *key,
                                                   u32 new_value, bool update, u32 *old_value_ptr)
{
  /*
   * int_map doesn't allow null pointer values, so make it not look like null
   * by setting a bit which will be discarded by the conversion to u32 on
   * retrieval.
   */
  unsigned long hack_value = (1ULL << 32) | new_value;
  void *old_ptr_value;
  int result;

  result = vdo_int_map_put((struct int_map *)map, (unsigned long)key, (void *)hack_value, update,
                           &old_ptr_value);
  if (old_value_ptr) {
    *old_value_ptr = (u32)(unsigned long)old_ptr_value;
  }
  return result;
}

static inline void vdo_ptr_u32_map_remove(struct ptr_u32_map *map, void *key)
{
  vdo_int_map_remove((struct int_map *)map, (unsigned long)key);
}
