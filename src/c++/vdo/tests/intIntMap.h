/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef INT_INT_MAP_H
#define INT_INT_MAP_H

#include "types.h"

/**
 * IntIntMap is a wrapper to allow an IntMap to hold integers instead of
 * objects.
 **/
typedef struct intIntMap IntIntMap;

/**
 * Construct an IntIntMap.
 *
 * @param [in]  initialCapacity  the number of entries the map should
 *                               initially be capable of holding (zero tells
 *                               the map to use its own small default)
 * @param [out] mapPtr           a pointer to hold the new IntIntMap
 *
 * @return VDO_SUCCESS or an error code
 **/
int makeIntIntMap(size_t initialCapacity, IntIntMap **mapPtr)
  __attribute__((warn_unused_result));

/**
 * Destroy an IntIntMap and NULL out the reference to it.
 *
 * @param mapPtr  A pointer to the IntIntMap to free
 **/
void freeIntIntMap(IntIntMap **mapPtr);

/**
 * Get the number of entries stored in an IntIntMap.
 *
 * @param map  the IntIntMap to query
 *
 * @return the number of entries in the map
 **/
size_t intIntMapSize(const IntIntMap *map)
  __attribute__((warn_unused_result));

/**
 * Retrieve the value associated with a given key from the IntIntMap.
 *
 * @param [in]  map       the IntIntMap to query
 * @param [in]  key       the key to look up
 * @param [out] valuePtr  A pointer to hold the value found, if any
 *
 * @return <code>true</code> if the key was in the map
 **/
bool intIntMapGet(IntIntMap *map, uint64_t key, uint64_t *valuePtr)
  __attribute__((warn_unused_result));

/**
 * Try to associate a value (an integer) with a key (also an integer) in an
 * IntIntMap. If the map already contains a mapping for the provided key, the
 * old value is only replaced with the specified value if update is true. In
 * either case the old value is returned. If the map does not already contain a
 * value for the specified key, the new value is added regardless of the value
 * of update.
 *
 * @param [in]  map           the IntIntMap to attempt to modify
 * @param [in]  key           the key with which to associate the new value
 * @param [in]  newValue      the value to be associated with the key
 * @param [in]  update        whether to overwrite an existing value
 * @param [out] wasMappedPtr  a pointer to store whether the key already had a value;
 *                            <code>NULL</code> may be provided if the caller doesn't
 *                            care
 * @param [out] oldValuePtr   a pointer in which to store the old value (if the
 *                            key was already mapped); <code>NULL</code> may be
 *                            provided if the caller does not need to know the
 *                            old value
 *
 * @return VDO_SUCCESS or an error code
 **/
int intIntMapPut(IntIntMap *map,
                 uint64_t   key,
                 uint64_t   newValue,
                 bool       update,
                 bool      *wasMappedPtr,
                 uint64_t  *oldValuePtr)
  __attribute__((warn_unused_result));

/**
 * Remove the mapping for a given key from an IntIntMap.
 *
 * @param [in]  map          the IntIntMap from which to remove the mapping
 * @param [in]  key          the key whose mapping is to be removed
 * @param [out] oldValuePtr  a pointer in which to store either the old value
 *                           (if the key was already mapped); <code>NULL</code>
 *                           may be provided if the caller does not need to
 *                           know the old value
 *
 * @return <code>true</code> if the key was mapped
 **/
bool intIntMapRemove(IntIntMap *map, uint64_t key, uint64_t *oldValuePtr);

#endif /* INT_INT_MAP_H */
