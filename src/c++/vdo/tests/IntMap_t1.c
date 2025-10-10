/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/limits.h>
#include <stdlib.h>

#include "memory-alloc.h"
#include "syscalls.h"

#include "int-map.h"

#include "vdoAsserts.h"

/**********************************************************************/
static void testEmptyMap(void)
{
  struct int_map *map;
  VDO_ASSERT_SUCCESS(vdo_int_map_create(0, &map));

  // Check the properties of the empty map.
  CU_ASSERT_EQUAL(0, vdo_int_map_size(map));
  CU_ASSERT_PTR_NULL(vdo_int_map_get(map, 0));

  // Try to remove the zero key--it should not be mapped.
  CU_ASSERT_PTR_NULL(vdo_int_map_remove(map, 0));

  // Try to remove a randomly-selected key--it should not be mapped.
  CU_ASSERT_PTR_NULL(vdo_int_map_remove(map, random()));

  vdo_int_map_free(vdo_forget(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void verifySingletonMap(struct int_map *map, uint64_t key, void *value)
{
  CU_ASSERT_EQUAL(1, vdo_int_map_size(map));
  CU_ASSERT_PTR_EQUAL(value, vdo_int_map_get(map, key));
}

/**********************************************************************/
static void testSingletonMap(void)
{
  struct int_map *map;
  VDO_ASSERT_SUCCESS(vdo_int_map_create(1, &map));

  // Add one entry with a randomly-selected key.
  int key = random();
  void *value = &key;
  void *oldValue = &value;
  VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, value, true, &oldValue));

  // The key must not have been mapped before.
  CU_ASSERT_PTR_NULL(oldValue);

  verifySingletonMap(map, key, value);

  // passing update=false should not overwrite an existing entry.
  char foo;
  void *value2 = &foo;
  void *oldValue2 = NULL;
  VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, value2, false, &oldValue2));
  CU_ASSERT_PTR_EQUAL(value, oldValue2);
  verifySingletonMap(map, key, value);

  if (key != 0) {
    // Try to remove the zero key--it should not be mapped.
    CU_ASSERT_PTR_NULL(vdo_int_map_remove(map, 0));
    verifySingletonMap(map, key, value);
  }

  // Try to remove a random key that is not the mapped key. In a small table,
  // this will frequently (1/N chance) have the same hash as the existing key.
  int bogusKey;
  do {
    bogusKey = random();
  } while (bogusKey == key);
  CU_ASSERT_PTR_NULL(vdo_int_map_remove(map, bogusKey));
  verifySingletonMap(map, key, value);

  // Replace the singleton key.
  void *value3 = &value;
  oldValue = &value;
  VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, value3, true, &oldValue));

  // The previous mapping value must be returned in oldValue.
  CU_ASSERT_PTR_EQUAL(value, oldValue);
  verifySingletonMap(map, key, value3);

  // Remove the singleton.
  CU_ASSERT_PTR_EQUAL(value3, vdo_int_map_remove(map, key));

  // The mapping must no longer be there.
  CU_ASSERT_EQUAL(0, vdo_int_map_size(map));
  CU_ASSERT_PTR_NULL(vdo_int_map_get(map, key));

  // Try to add the value again.
  VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, value2, false, &oldValue));
  CU_ASSERT_PTR_EQUAL(NULL, oldValue);
  verifySingletonMap(map, key, value2);

  vdo_int_map_free(vdo_forget(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void test16BitMap(void)
{
  struct int_map *map;
  VDO_ASSERT_SUCCESS(vdo_int_map_create(U16_MAX + 1, &map));

  uint16_t *values;
  VDO_ASSERT_SUCCESS(vdo_allocate(65536, "16-bit values", &values));
  for (int i = 0; i <= U16_MAX; i++) {
    values[i] = i;
  }

  // Create an identity map of [0..65535] -> [0..65535].
  for (int key = 0; key <= U16_MAX; key++) {
    CU_ASSERT_EQUAL(key, vdo_int_map_size(map));
    CU_ASSERT_PTR_NULL(vdo_int_map_get(map, key));
    VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, &values[key], true, NULL));
    CU_ASSERT_PTR_EQUAL(&values[key], vdo_int_map_get(map, key));
  }
  CU_ASSERT_EQUAL(1 << 16, vdo_int_map_size(map));

  // Remove the odd-numbered keys.
  for (int key = 1; key <= U16_MAX; key += 2) {
    CU_ASSERT_PTR_EQUAL(&values[key], vdo_int_map_remove(map, key));
    CU_ASSERT_PTR_NULL(vdo_int_map_get(map, key));
  }
  CU_ASSERT_EQUAL(1 << 15, vdo_int_map_size(map));

  // Re-map everything to its complement: 0->65535, 1->65534, etc.
  for (int key = 0; key <= U16_MAX; key++) {
    void *value = vdo_int_map_get(map, key);
    if ((key % 2) == 0) {
      CU_ASSERT_PTR_EQUAL(&values[key], value);
    } else {
      CU_ASSERT_PTR_NULL(value);
    }
    void *newValue = &values[U16_MAX - key];
    VDO_ASSERT_SUCCESS(vdo_int_map_put(map, key, newValue, true, NULL));
  }

  // Verify the mapping.
  CU_ASSERT_EQUAL(1 << 16, vdo_int_map_size(map));
  for (int key = 0; key <= U16_MAX; key++) {
    CU_ASSERT_PTR_EQUAL(&values[U16_MAX - key], vdo_int_map_get(map, key));
  }

  // Remove everything.
  for (int key = 0; key <= U16_MAX; key++) {
    CU_ASSERT_PTR_EQUAL(&values[U16_MAX - key], vdo_int_map_remove(map, key));
    CU_ASSERT_PTR_NULL(vdo_int_map_get(map, key));
    CU_ASSERT_EQUAL(U16_MAX - key, vdo_int_map_size(map));
  }
  CU_ASSERT_EQUAL(0, vdo_int_map_size(map));

  free(values);
  vdo_int_map_free(vdo_forget(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static void testSteadyState(void)
{
  static size_t SIZE = 10 * 1000;

  struct int_map *map;
  VDO_ASSERT_SUCCESS(vdo_int_map_create(0, &map));

  // Fill the map with mappings of { 0 -> 1 }, { 1 -> 2 }, etc.
  for (size_t i = 0; i < SIZE; i++) {
    CU_ASSERT_EQUAL(i, vdo_int_map_size(map));
    VDO_ASSERT_SUCCESS(vdo_int_map_put(map, i, (void *) (i + 1), true, NULL));
  }

  // Remove mappings one by one and replace them with a different key,
  // exercising the operation of the map at a steady-state of N entries.
  for (size_t i = 0; i < (10 * SIZE); i++) {
    CU_ASSERT_PTR_EQUAL((void *) (i + 1), vdo_int_map_remove(map, i));
    VDO_ASSERT_SUCCESS(vdo_int_map_put(map,
                                       SIZE + i,
                                       (void *) (SIZE + i + 1),
                                       true,
                                       NULL));
    CU_ASSERT_EQUAL(SIZE, vdo_int_map_size(map));
  }

  vdo_int_map_free(vdo_forget(map));
  CU_ASSERT_PTR_NULL(map);
}

/**********************************************************************/
static CU_TestInfo tests[] = {
  { "empty map",        testEmptyMap        },
  { "singleton map",    testSingletonMap    },
  { "16-bit map",       test16BitMap        },
  { "steady-state map", testSteadyState     },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name                     = "IntMap_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
