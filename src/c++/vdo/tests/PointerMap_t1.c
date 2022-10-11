/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/random.h>
#include <stdlib.h>

#include "assertions.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "syscalls.h"

#include "pointer-map.h"

/**********************************************************************/
static bool compareKeys(const void *thisKey, const void *thatKey)
{
  if ((thisKey == NULL) || (thatKey == NULL)) {
    return (thisKey == thatKey);
  }
  return (strcmp(thisKey, thatKey) == 0);
}

/**
 * Calculate the FNV-1a 32-bit hash of a null-terminated string.
 *
 * @param string  The string to hash
 *
 * @return a hash code of the string contents
 **/
static uint32_t hashFNV32(const char *string)
{
  // FNV-1a hash constants from http://www.isthe.com/chongo/tech/comp/fnv/
  const uint32_t FNV32_PRIME  = 16777619;
  const uint32_t FNV32_OFFSET = 2166136261;

  uint32_t hash = FNV32_OFFSET;
  if (string != NULL) {
    while (*string != '\0') {
      hash ^= *string++;
      hash *= FNV32_PRIME;
    }
  }
  return hash;
}

/**********************************************************************/
static uint32_t hashKey(const void *key)
{
  return hashFNV32((const char *) key);
}

/**********************************************************************/
static void testEmptyMap(void)
{
  struct pointer_map *map;
  UDS_ASSERT_SUCCESS(make_pointer_map(0, 0, compareKeys, hashKey, &map));

  // Check the properties of the empty map.
  CU_ASSERT_EQUAL(0, pointer_map_size(map));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, NULL));

  // Try to remove the NULL key--it should not be mapped.
  CU_ASSERT_PTR_NULL(pointer_map_remove(map, NULL));

  // Try to remove the empty string--it should not be mapped.
  CU_ASSERT_PTR_NULL(pointer_map_remove(map, ""));

  free_pointer_map(UDS_FORGET(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void verifySingletonMap(struct pointer_map *map,
                               const char         *key,
                               void               *value)
{
  CU_ASSERT_EQUAL(1, pointer_map_size(map));
  CU_ASSERT_PTR_EQUAL(value, pointer_map_get(map, key));
}

/**********************************************************************/
static void testNullKey(void)
{
  struct pointer_map *map;
  UDS_ASSERT_SUCCESS(make_pointer_map(1, 0, compareKeys, hashKey, &map));

  const char *nullKey    = NULL;
  const char *emptyKey   = "";
  void       *nullValue  = "null string";
  void       *emptyValue = "empty string";

  // The NULL key and the empty string should have the same hash code, but
  // must be treated as distinct keys.
  CU_ASSERT_EQUAL(hashKey(nullKey), hashKey(emptyKey));
  CU_ASSERT_FALSE(compareKeys(nullKey, emptyKey));
  CU_ASSERT_FALSE(compareKeys(emptyKey, nullKey));

  // Map NULL to "null string".
  void *oldValue = &nullValue;
  UDS_ASSERT_SUCCESS(pointer_map_put(map, nullKey, nullValue, true, &oldValue));

  // The key must not have been mapped before.
  CU_ASSERT_PTR_NULL(oldValue);

  verifySingletonMap(map, nullKey, nullValue);

  // The NULL key in the map must not be found by the empty key.
  CU_ASSERT_PTR_NULL(pointer_map_get(map, emptyKey));
  CU_ASSERT_PTR_NULL(pointer_map_remove(map, emptyKey));
  verifySingletonMap(map, nullKey, nullValue);

  // Unmap the NULL key.
  CU_ASSERT_PTR_EQUAL(nullValue, pointer_map_remove(map, nullKey));

  // The mapping must no longer be there.
  CU_ASSERT_EQUAL(0, pointer_map_size(map));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, nullKey));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, emptyKey));

  // Map "" to "empty string".
  oldValue = &emptyValue;
  UDS_ASSERT_SUCCESS(pointer_map_put(map, emptyKey, emptyValue, true,
                                     &oldValue));

  // The key must not have been mapped before.
  CU_ASSERT_PTR_NULL(oldValue);

  verifySingletonMap(map, emptyKey, emptyValue);

  // The empty key in the map must not be found by the NULL key.
  CU_ASSERT_PTR_NULL(pointer_map_get(map, nullKey));
  CU_ASSERT_PTR_NULL(pointer_map_remove(map, nullKey));
  verifySingletonMap(map, emptyKey, emptyValue);

  // Unmap the empty key.
  CU_ASSERT_PTR_EQUAL(emptyValue, pointer_map_remove(map, emptyKey));

  // The mapping must no longer be there.
  CU_ASSERT_EQUAL(0, pointer_map_size(map));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, nullKey));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, emptyKey));

  free_pointer_map(UDS_FORGET(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void testSingletonMap(void)
{
  struct pointer_map *map;
  UDS_ASSERT_SUCCESS(make_pointer_map(1, 0, compareKeys, hashKey, &map));

  // Add one entry with a randomly-selected key.
  char key[10] = { 0, };
  get_random_bytes(key, sizeof(key) - 1);
  void *value = &key;
  void *oldValue = &value;
  UDS_ASSERT_SUCCESS(pointer_map_put(map, key, value, true, &oldValue));

  // The key must not have been mapped before.
  CU_ASSERT_PTR_NULL(oldValue);

  verifySingletonMap(map, key, value);

  // passing update=false should not overwrite an existing entry.
  char foo;
  void *value2 = &foo;
  void *oldValue2 = NULL;
  UDS_ASSERT_SUCCESS(pointer_map_put(map, key, value2, false, &oldValue2));
  CU_ASSERT_PTR_EQUAL(value, oldValue2);
  verifySingletonMap(map, key, value);

  if (strlen(key) != 0) {
    // Try to remove the empty key--it should not be mapped.
    CU_ASSERT_PTR_NULL(pointer_map_remove(map, NULL));
    verifySingletonMap(map, key, value);
  }

  // Try to remove a random key that is not the mapped key. In a small table,
  // this will frequently (1/N chance) have the same hash as the existing key.
  char bogusKey[10] = { 0, };
  do {
    get_random_bytes(bogusKey, sizeof(bogusKey) - 1);
  } while (strcmp(key, bogusKey) == 0);
  CU_ASSERT_PTR_NULL(pointer_map_remove(map, bogusKey));
  verifySingletonMap(map, key, value);

  // Replace the singleton key.
  void *value3 = &value;
  char key3[10];
  strncpy(key3, key, sizeof(key3));
  oldValue = &value;
  UDS_ASSERT_SUCCESS(pointer_map_put(map, key3, value3, true, &oldValue));

  // The previous mapping value must be returned in oldValue.
  CU_ASSERT_PTR_EQUAL(value, oldValue);
  verifySingletonMap(map, key3, value3);

  /*
   * Check that, when update=true, the old key is not retained by the map.
   * (Given the key/value non-ownership of the map, removing the old value
   * should also remove the old key, since the keys will likely be properties
   * of the value.) Temporarily mutating the old key so the two keys are
   * different should suffice.
   */
  if (strlen(key) != 0) {
    key[0] = ~key[0];
    CU_ASSERT_STRING_NOT_EQUAL(key, key3);
    verifySingletonMap(map, key3, value3);
    CU_ASSERT_PTR_NULL(pointer_map_get(map, key));
    key[0] = ~key[0];
    verifySingletonMap(map, key, value3);
  }

  // Remove the singleton.
  CU_ASSERT_PTR_EQUAL(value3, pointer_map_remove(map, key3));

  // The mapping must no longer be there.
  CU_ASSERT_EQUAL(0, pointer_map_size(map));
  CU_ASSERT_PTR_NULL(pointer_map_get(map, key3));

  // Try to add the value again.
  UDS_ASSERT_SUCCESS(pointer_map_put(map, key, value2, false, &oldValue));
  CU_ASSERT_PTR_EQUAL(NULL, oldValue);
  verifySingletonMap(map, key, value2);

  free_pointer_map(UDS_FORGET(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static char *toKeyString(unsigned int key)
{
  char *keyString;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &keyString, "[%d]", key));
  return keyString;
}

/**********************************************************************/
static void test16BitMap(void)
{
  struct pointer_map *map;
  UDS_ASSERT_SUCCESS(make_pointer_map(UINT16_MAX + 1, 0, compareKeys, hashKey,
                                      &map));

  char **keys;
  uint16_t *values;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(UINT16_MAX + 1, char *, "key string array",
                                  &keys));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(UINT16_MAX + 1, uint16_t, "16-bit values",
                                  &values));
  for (int i = 0; i <= UINT16_MAX; i++) {
    keys[i] = toKeyString(i);
    values[i] = i;
  }

  // Create an identity map of [0..65535] -> [0..65535].
  for (int key = 0; key <= UINT16_MAX; key++) {
    CU_ASSERT_EQUAL(key, pointer_map_size(map));
    CU_ASSERT_PTR_NULL(pointer_map_get(map, keys[key]));
    UDS_ASSERT_SUCCESS(pointer_map_put(map, keys[key], &values[key],
                                       true, NULL));
    CU_ASSERT_PTR_EQUAL(&values[key], pointer_map_get(map, keys[key]));
  }
  CU_ASSERT_EQUAL(1 << 16, pointer_map_size(map));

  // Remove the odd-numbered keys.
  for (int key = 1; key <= UINT16_MAX; key += 2) {
    CU_ASSERT_PTR_EQUAL(&values[key], pointer_map_remove(map, keys[key]));
    CU_ASSERT_PTR_NULL(pointer_map_get(map, keys[key]));
  }
  CU_ASSERT_EQUAL(1 << 15, pointer_map_size(map));

  // Re-map everything to its complement: 0->65535, 1->65534, etc.
  for (int key = 0; key <= UINT16_MAX; key++) {
    void *value = pointer_map_get(map, keys[key]);
    if ((key % 2) == 0) {
      CU_ASSERT_PTR_EQUAL(&values[key], value);
    } else {
      CU_ASSERT_PTR_NULL(value);
    }
    void *newValue = &values[UINT16_MAX - key];
    UDS_ASSERT_SUCCESS(pointer_map_put(map, keys[key], newValue, true, NULL));
  }

  // Verify the mapping.
  CU_ASSERT_EQUAL(1 << 16, pointer_map_size(map));
  for (int key = 0; key <= UINT16_MAX; key++) {
    CU_ASSERT_PTR_EQUAL(&values[UINT16_MAX - key],
                        pointer_map_get(map, keys[key]));
  }

  // Remove everything.
  for (int key = 0; key <= UINT16_MAX; key++) {
    CU_ASSERT_PTR_EQUAL(&values[UINT16_MAX - key],
                        pointer_map_remove(map, keys[key]));
    CU_ASSERT_PTR_NULL(pointer_map_get(map, keys[key]));
    CU_ASSERT_EQUAL(UINT16_MAX - key, pointer_map_size(map));
    UDS_FREE(keys[key]);
  }
  CU_ASSERT_EQUAL(0, pointer_map_size(map));

  UDS_FREE(keys);
  UDS_FREE(values);
  free_pointer_map(UDS_FORGET(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void testSteadyState(void)
{
  static size_t SIZE = 10 * 1000;

  struct pointer_map *map;
  UDS_ASSERT_SUCCESS(make_pointer_map(0, 0, compareKeys, hashKey, &map));

  // Fill the map with trivial mappings of { "[0]" -> "[0]" }, etc
  for (size_t i = 0; i < SIZE; i++) {
    CU_ASSERT_EQUAL(i, pointer_map_size(map));
    char *key = toKeyString(i);
    UDS_ASSERT_SUCCESS(pointer_map_put(map, key, key, true, NULL));
  }

  // Remove mappings one by one and replace them with a different key,
  // exercising the operation of the map at a steady-state of SIZE entries.
  for (size_t i = 0; i < (10 * SIZE); i++) {
    char *probeKey = toKeyString(i);
    void *value = pointer_map_remove(map, probeKey);
    CU_ASSERT_STRING_EQUAL(probeKey, value);
    UDS_FREE(probeKey);
    UDS_FREE(value);

    char *key = toKeyString(SIZE + i);
    UDS_ASSERT_SUCCESS(pointer_map_put(map, key, key, true, NULL));
    CU_ASSERT_EQUAL(SIZE, pointer_map_size(map));
  }

  // Free the entries remaining in the map.
  for (size_t i = 0; i < SIZE; i++) {
    char *probeKey = toKeyString(i + (10 * SIZE));
    void *value = pointer_map_remove(map, probeKey);
    CU_ASSERT_STRING_EQUAL(probeKey, value);
    UDS_FREE(probeKey);
    UDS_FREE(value);
  }

  free_pointer_map(UDS_FORGET(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "empty map",        testEmptyMap        },
  { "null key",         testNullKey         },
  { "singleton map",    testSingletonMap    },
  { "16-bit map",       test16BitMap        },
  { "steady-state map", testSteadyState     },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "PointerMap_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
