/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "intIntMap.h"

#include <linux/list.h>

#include "memory-alloc.h"
#include "syscalls.h"

#include "int-map.h"
#include "status-codes.h"
#include "types.h"

typedef struct {
  struct list_head node;
  uint64_t         value;
} IntHolder;

/**
 * IntIntMap is a wrapper to allow an int_map to hold integers instead of
 * objects.
 **/
struct intIntMap {
  struct int_map   *map;
  struct list_head  holders;
};

/**********************************************************************/
int makeIntIntMap(size_t initialCapacity, IntIntMap **mapPtr)
{
  IntIntMap *intIntMap;
  int result = uds_allocate(1, IntIntMap, __func__, &intIntMap);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = vdo_int_map_create(initialCapacity, 0, &intIntMap->map);
  if (result != VDO_SUCCESS) {
    uds_free(intIntMap);
    return result;
  }

  INIT_LIST_HEAD(&intIntMap->holders);
  *mapPtr = intIntMap;
  return result;
}

/**********************************************************************/
void freeIntIntMap(IntIntMap **mapPtr)
{
  IntIntMap *intIntMap = *mapPtr;
  if (intIntMap == NULL) {
    return;
  }

  vdo_int_map_free(uds_forget(intIntMap->map));

  IntHolder *holder, *tmp;
  list_for_each_entry_safe_reverse(holder, tmp, &intIntMap->holders, node) {
    list_del(&holder->node);
    uds_free(holder);
  }

  uds_free(intIntMap);
  *mapPtr = NULL;
}

/**********************************************************************/
size_t intIntMapSize(const IntIntMap *map)
{
  return vdo_int_map_size(map->map);
}

/**********************************************************************/
bool intIntMapGet(IntIntMap *map, uint64_t key, uint64_t *value)
{
  IntHolder *holder = vdo_int_map_get(map->map, key);
  if (holder == NULL) {
    return false;
  }

  *value = holder->value;
  return true;
}

/**********************************************************************/
int intIntMapPut(IntIntMap *map,
                 uint64_t   key,
                 uint64_t   newValue,
                 bool       update,
                 bool      *wasMappedPtr,
                 uint64_t  *oldValuePtr)
{
  IntHolder *newHolder;
  int result = uds_allocate(1, IntHolder, __func__, &newHolder);
  if (result != VDO_SUCCESS) {
    return result;
  }

  newHolder->value = newValue;

  IntHolder *holder;
  result
    = vdo_int_map_put(map->map, key, newHolder, update, (void **) &holder);
  if (result != VDO_SUCCESS) {
    uds_free(newHolder);
    return result;
  }

  if (wasMappedPtr != NULL) {
    *wasMappedPtr = (holder != NULL);
  }

  if (holder != NULL) {
    if (oldValuePtr != NULL) {
      *oldValuePtr = holder->value;
    }

    if (!update) {
      uds_free(newHolder);
      return true;
    }

    list_del(&holder->node);
    uds_free(holder);
  }

  INIT_LIST_HEAD(&newHolder->node);
  list_add_tail(&newHolder->node, &map->holders);
  return result;
}

/**********************************************************************/
bool intIntMapRemove(IntIntMap *map, uint64_t key, uint64_t *oldValuePtr)
{
  IntHolder *holder = vdo_int_map_remove(map->map, key);
  if (holder == NULL) {
    return false;
  }

  if (oldValuePtr != NULL) {
    *oldValuePtr = holder->value;
  }

  list_del(&holder->node);
  uds_free(holder);
  return true;
}
