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
  int result = UDS_ALLOCATE(1, IntIntMap, __func__, &intIntMap);
  if (result != VDO_SUCCESS) {
    return result;
  }

  result = make_int_map(initialCapacity, 0, &intIntMap->map);
  if (result != VDO_SUCCESS) {
    UDS_FREE(intIntMap);
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

  free_int_map(UDS_FORGET(intIntMap->map));
  while (!list_empty(&intIntMap->holders)) {
    struct list_head *entry = intIntMap->holders.prev;
    list_del(entry);
    UDS_FREE(entry);
  }

  UDS_FREE(intIntMap);
  *mapPtr = NULL;
}

/**********************************************************************/
size_t intIntMapSize(const IntIntMap *map)
{
  return int_map_size(map->map);
}

/**********************************************************************/
bool intIntMapGet(IntIntMap *map, uint64_t key, uint64_t *value)
{
  IntHolder *holder = int_map_get(map->map, key);
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
  int result = UDS_ALLOCATE(1, IntHolder, __func__, &newHolder);
  if (result != VDO_SUCCESS) {
    return result;
  }

  newHolder->value = newValue;

  IntHolder *holder;
  result = int_map_put(map->map, key, newHolder, update, (void **) &holder);
  if (result != VDO_SUCCESS) {
    UDS_FREE(newHolder);
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
      UDS_FREE(newHolder);
      return true;
    }

    list_del(&holder->node);
    UDS_FREE(holder);
  }

  INIT_LIST_HEAD(&newHolder->node);
  list_add_tail(&newHolder->node, &map->holders);
  return result;
}

/**********************************************************************/
bool intIntMapRemove(IntIntMap *map, uint64_t key, uint64_t *oldValuePtr)
{
  IntHolder *holder = int_map_remove(map->map, key);
  if (holder == NULL) {
    return false;
  }

  if (oldValuePtr != NULL) {
    *oldValuePtr = holder->value;
  }

  list_del(&holder->node);
  UDS_FREE(holder);
  return true;
}
