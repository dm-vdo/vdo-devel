// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"
#include "io-factory.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "random.h"
#include "testPrototypes.h"

enum { ONE_ZONE = 1 };  // We generally test with one zone

/**
 * Create a new block name.
 *
 * @param [out] name  the new block name.
 **/
static void createBlockName(struct uds_record_name *name)
{
  static uint64_t counter = 0;
  memset(name, 0, UDS_RECORD_NAME_SIZE);
  put_unaligned_le64(counter, name->name);
  counter++;
}

/*
 * We want to prevent inlining of these assertFoo() methods to make
 * debugging easier.
 */

/**********************************************************************/
static noinline void assertKeyValue(const struct delta_index_entry *entry,
                                    unsigned int key, unsigned int value)
{
  // We expect the entry to reflect a found key-value pair.  This can be the
  // result of a successful get_delta_index_entry, or a put_delta_index_entry
  // just wrote the entry, or set_delta_entry_value just modified the entry.
  CU_ASSERT_FALSE(entry->at_end);
  CU_ASSERT_EQUAL(entry->key, key);
  CU_ASSERT_EQUAL(get_delta_entry_value(entry), value);
}

/**********************************************************************/
static noinline void assertSavedValid(const struct delta_index *di)
{
  struct delta_index_entry entry;
  UDS_ASSERT_SUCCESS(start_delta_index_search(di, 0, 0, &entry));
  const struct delta_list *deltaList = entry.delta_list;
  unsigned int saveKey    = deltaList->save_key;
  unsigned int saveOffset = deltaList->save_offset;
  bool found = false;
  do {
    UDS_ASSERT_SUCCESS(next_delta_index_entry(&entry));
    if ((saveKey == entry.key - entry.delta) && (saveOffset == entry.offset)) {
      found = true;
    }
  } while (!entry.at_end);
  CU_ASSERT_TRUE(found);
}

/**********************************************************************/
static noinline void assertSavedAt(const struct delta_index_entry *entry)
{
  const struct delta_list *deltaList = entry->delta_list;
  // We expect the saved entry offset to refer to this entry
  CU_ASSERT_EQUAL(deltaList->save_key,    entry->key - entry->delta);
  CU_ASSERT_EQUAL(deltaList->save_offset, entry->offset);
}

/**********************************************************************/
static noinline void assertSavedBefore(const struct delta_index_entry *entry)
{
  const struct delta_list *deltaList = entry->delta_list;
  // We expect the saved entry offset to refer to a prior entry.
  CU_ASSERT(deltaList->save_key <= entry->key);
  CU_ASSERT(deltaList->save_offset < entry->offset);
}

/**
 * Validate the delta index.
 *
 * @param delta_index  The delta index
 **/
static void validateDeltaIndex(const struct delta_index *delta_index)
{
  unsigned int z;

 for (z = 0; z < delta_index->zone_count; z++) {
    validateDeltaLists(&delta_index->delta_zones[z]);
  }
}

/**
 * Test initialization
 **/
static void initializationTest(void)
{
  struct delta_index di;
  unsigned int numLists = 1024;
  unsigned int meanDelta = 1024;
  int numPayloadBits = 10;
  size_t memSize = 16 * MEGABYTE;

  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, numLists, meanDelta,
                                            numPayloadBits, memSize, 'm'));
  uninitialize_delta_index(&di);
  uninitialize_delta_index(&di);
}

/**
 * Test basic record get/put/remove
 **/
static void basicTest(void)
{
  struct delta_index di;
  struct delta_index_entry entry;
  enum { NUM_LISTS = 1 };
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, NUM_LISTS, 256, 8,
                                            2 * MEGABYTE, 'm'));

  // Should not find a record with key 0 in an empty list
  struct uds_record_name name0;
  createBlockName(&name0);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 0, name0.name, &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);

  // Insert a record with key 1
  struct uds_record_name name1;
  createBlockName(&name1);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 1, name1.name, &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, 1, 99, NULL));
  CU_ASSERT_EQUAL(entry.key, 1);
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  CU_ASSERT_EQUAL(get_delta_entry_value(&entry), 99);

  // Should not find a record with key 0
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 0, name0.name, &entry));
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);

  // Should find the record with key 1
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 1, name1.name, &entry));
  CU_ASSERT_EQUAL(entry.key, 1);
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  CU_ASSERT_EQUAL(get_delta_entry_value(&entry), 99);

  // Modify its payload
  UDS_ASSERT_SUCCESS(set_delta_entry_value(&entry, 42));
  CU_ASSERT_EQUAL(entry.key, 1);
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  CU_ASSERT_EQUAL(get_delta_entry_value(&entry), 42);

  // Should not find a record with key 2
  struct uds_record_name name2;
  createBlockName(&name2);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 2, name2.name, &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);

  // Remove the record with key 1
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 1, name1.name, &entry));
  CU_ASSERT_EQUAL(entry.key, 1);
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  CU_ASSERT_EQUAL(get_delta_entry_value(&entry), 42);
  UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));

  // Should not find a record with key 1
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 1, name1.name, &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);

  uninitialize_delta_index(&di);
}

/**
 * Test record sizes, using expectations based on a mean delta of 1024
 **/
static void recordSizeTest(void)
{
  static const struct {
    unsigned int delta;
    unsigned int expectedSize;
  } table[] = {
    {        0, 10 },
    {        1, 10 },
    {        2, 10 },
    {        4, 10 },
    {        8, 10 },
    {      313, 10 },
    {      314, 11 },
    {     1023, 11 },
    {     1024, 12 },
    {     1733, 12 },
    {     1734, 13 },
    {     2443, 13 },
    {     2444, 14 },
    {     3153, 14 },
    {     3154, 15 },
    {     3863, 15 },
    {     3864, 16 },
    {     4573, 16 },
    {     4574, 17 },
    {     5283, 17 },
    {     5284, 18 },
    {     5993, 18 },
    {     5994, 19 },
    {     6703, 19 },
    {     6704, 20 },
    { 0x1FFFFF, 2964 }
  };
  static const unsigned int tableCount = sizeof(table) / sizeof(table[0]);

  struct delta_index di;
  enum { NUM_LISTS = 1, PAYLOAD_BITS = 4 };
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, NUM_LISTS, 1024,
                                            PAYLOAD_BITS, 2 * MEGABYTE, 'm'));

  unsigned int filler, i;
  for (filler = 0; filler < 2; filler++) {
    for (i = 0; i < tableCount; i++) {
      struct delta_index_entry entry;
      struct delta_index_stats stats;

      // Build the block name, and make up a key/payload to use
      struct uds_record_name name;
      createBlockName(&name);
      unsigned int key = table[i].delta;
      unsigned int payload = i & ((1 << PAYLOAD_BITS) - 1);

      // The delta index starts out empty
      get_delta_index_stats(&di, &stats);
      CU_ASSERT_EQUAL(stats.record_count, 0);

      // Fill the delta memory with the filler value
      memset(di.delta_zones[0].memory, filler ? 0xFF : 0,
             di.delta_zones[0].size);

      // Create the record
      UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, key, name.name, &entry));
      CU_ASSERT_TRUE(entry.at_end);
      CU_ASSERT_FALSE(entry.is_collision);
      UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, key, payload, NULL));
      CU_ASSERT_EQUAL(entry.key, key);
      CU_ASSERT_FALSE(entry.at_end);
      CU_ASSERT_FALSE(entry.is_collision);
      CU_ASSERT_EQUAL(entry.delta, key);

      // Derive key_bits by subtracting the sizes of the other two fields
      // from the total.
      unsigned int keyBits = (entry.entry_bits - entry.value_bits);
      CU_ASSERT_EQUAL(keyBits, table[i].expectedSize);
      CU_ASSERT_EQUAL(get_delta_entry_value(&entry), payload);

      // The delta index now has one entry
      get_delta_index_stats(&di, &stats);
      CU_ASSERT_EQUAL(stats.record_count, 1);

      // Verify that we find the record we inserted
      UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, key, name.name, &entry));
      assertKeyValue(&entry, key, payload);
      CU_ASSERT_FALSE(entry.is_collision);

      // Remove the record
      UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));

      // The delta index ends up empty
      get_delta_index_stats(&di, &stats);
      CU_ASSERT_EQUAL(stats.record_count, 0);
    }
  }

  uninitialize_delta_index(&di);
}

/**
 * Test a list of entries in a delta index.
 *
 * @param keys                The keys to put/remove
 * @param numKeys             The expected number of names
 * @param expectedCollisions  The number of keys that will result in
 *                            volume index collisions
 */
static void testAddRemove(const unsigned int *keys, unsigned int numKeys,
                          unsigned int expectedCollisions)
{
  struct delta_index di;
  struct delta_index_stats stats;
  enum { NUM_LISTS = 1, PAYLOAD_BITS = 4 };
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, NUM_LISTS, 1024,
                                            PAYLOAD_BITS, 2 * MEGABYTE, 'm'));
  CU_ASSERT_EQUAL(di.list_count, NUM_LISTS);
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, 0);

  struct delta_index_entry entry;
  struct uds_record_name *names;
  bool *collides;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numKeys, struct uds_record_name, __func__,
                                  &names));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numKeys, bool, __func__, &collides));

  // Put all the records in the specified order
  unsigned int i;
  for (i = 0; i < numKeys; i++) {
    collides[i] = false;
    createBlockName(&names[i]);
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, keys[i], names[i].name,
                                             &entry));
    if (expectedCollisions == 0) {
      CU_ASSERT_FALSE(entry.is_collision);
    } else if (!entry.at_end && entry.key == keys[i]) {
      collides[i] = true;
    }
    UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, keys[i], i,
                                             collides[i]
                                               ? names[i].name : NULL));
    assertKeyValue(&entry, keys[i], i);
    if (collides[i]) {
      CU_ASSERT_TRUE(entry.is_collision);
    } else {
      CU_ASSERT_FALSE(entry.is_collision);
    }
  }
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, numKeys);
  CU_ASSERT_EQUAL(stats.collision_count, expectedCollisions);
  validateDeltaIndex(&di);

  // Get all the records in the specified order
  for (i = 0; i < numKeys; i++) {
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, keys[i], names[i].name,
                                             &entry));
    assertKeyValue(&entry, keys[i], i);
    if (collides[i]) {
      CU_ASSERT_TRUE(entry.is_collision);
    } else {
      CU_ASSERT_FALSE(entry.is_collision);
    }
  }
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, numKeys);
  CU_ASSERT_EQUAL(stats.collision_count, expectedCollisions);
  validateDeltaIndex(&di);

  // Remove all the records in the specified order
  for (i = 0; i < numKeys; i++) {
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, keys[i], names[i].name,
                                             &entry));
    assertKeyValue(&entry, keys[i], i);
    UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
  }
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, 0);
  CU_ASSERT_EQUAL(stats.collision_count, 0);
  validateDeltaIndex(&di);

  // Get all the records in the specified order, expecting to not find them
  for (i = 0; i < numKeys; i++) {
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, keys[i], names[i].name,
                                             &entry));
    CU_ASSERT_TRUE(entry.at_end || (keys[i] != entry.key));
  }
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, 0);
  CU_ASSERT_EQUAL(stats.collision_count, 0);

  uninitialize_delta_index(&di);
  UDS_FREE(names);
  UDS_FREE(collides);
}

/**
 * Test non-colliding entries
 **/
static void noCollisionsTest(void)
{
  static const unsigned int keys[] = {
    0u, 0x8000u, 0x7FFFu, 0xFFFFu
  };
  static const unsigned int keyCount = sizeof(keys) / sizeof(keys[0]);
  testAddRemove(keys, keyCount, 0);
}

/**
 * Test colliding entries
 **/
static void collisionsTest(void)
{
  static const unsigned int keys[] = {
         0u,      0u,      0u,      0u,
    0x8000u, 0x8000u, 0x8000u, 0x8000u,
    0x7FFFu, 0x7FFFu, 0x7FFFu, 0x7FFFu,
    0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu
  };
  static const unsigned int keyCount = sizeof(keys) / sizeof(keys[0]);
  testAddRemove(keys, keyCount, 3 * 4);
}

/**
 * Test colliding entries, in a different order
 **/
static void interleavedTest(void)
{
  static const unsigned int keys[] = {
    0u, 0x8000u, 0x7FFFu, 0xFFFFu,
    0u, 0x8000u, 0x7FFFu, 0xFFFFu,
    0u, 0x8000u, 0x7FFFu, 0xFFFFu,
    0u, 0x8000u, 0x7FFFu, 0xFFFFu
  };
  static const unsigned int keyCount = sizeof(keys) / sizeof(keys[0]);
  testAddRemove(keys, keyCount, 3 * 4);
}

/**
 * Test colliding entries, in reversed order
 **/
static void reversedTest(void)
{
  static const unsigned int keys[] = {
    0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu,
    0x7FFFu, 0x7FFFu, 0x7FFFu, 0x7FFFu,
    0x8000u, 0x8000u, 0x8000u, 0x8000u,
         0u,      0u,      0u,      0u
  };
  static const unsigned int keyCount = sizeof(keys) / sizeof(keys[0]);
  testAddRemove(keys, keyCount, 3 * 4);
}

/**
 * Test delta list overflow
 **/
static void overflowTest(void)
{
  struct delta_index di;
  struct delta_index_entry entry;
  struct delta_index_stats stats;
  enum { NUM_LISTS = 1 };
  enum { PAYLOAD_BITS = 8 };
  enum { PAYLOAD_MASK = (1 << PAYLOAD_BITS) - 1 };
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, NUM_LISTS, 256,
                                            PAYLOAD_BITS, 2 * MEGABYTE, 'm'));
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.record_count, 0);
  CU_ASSERT_EQUAL(stats.overflow_count, 0);

  // Insert a record with key 0
  struct uds_record_name name;
  createBlockName(&name);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 0, name.name, &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, 0, 0, NULL));
  assertKeyValue(&entry, 0, 0);
  CU_ASSERT_FALSE(entry.is_collision);

  // How big was that entry?  We expect that all subsequent entries have
  // the same size, and compute the expected number of entries accordingly.
  int entrySize = get_delta_zone_bits_used(&di, 0);
  unsigned int entryCount = U16_MAX / entrySize;

  // Fill the index with more records, each with a delta of 1
  unsigned int key;
  for (key = 1; key < entryCount; key++) {
    createBlockName(&name);
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, key, name.name, &entry));
    CU_ASSERT_TRUE(entry.at_end);
    CU_ASSERT_FALSE(entry.is_collision);
    UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, key, key & PAYLOAD_MASK,
                                             NULL));
    assertKeyValue(&entry, key, key & PAYLOAD_MASK);
    CU_ASSERT_FALSE(entry.is_collision);
  }
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.overflow_count, 0);

  // Insert one more record, expecting to overflow the index
  createBlockName(&name);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, entryCount, name.name,
                                           &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  CU_ASSERT_EQUAL(put_delta_index_entry(&entry, entryCount,
                                        entryCount & PAYLOAD_MASK, NULL),
                  UDS_OVERFLOW);
  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.overflow_count, 1);

  // Now make sure we can continue to use the delta list that overflowed.
  // Look for all the records that were successfully inserted.
  for (key = 1; key < entryCount; key++) {
    createBlockName(&name);
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, key, name.name, &entry));
    assertKeyValue(&entry, key, key & PAYLOAD_MASK);
    CU_ASSERT_FALSE(entry.is_collision);

    // Delete half of the records.  Make sure to keep the one with key==0.
    if ((key & 1) != 0) {
      UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
    }

    // Some of the time, look for key 0.  We desire the side effect that
    // this search will force the next search to start at the beginning of
    // the delta list.
    if ((key & 2) != 0) {
      createBlockName(&name);
      UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 0, name.name, &entry));
      assertKeyValue(&entry, 0, 0);
      CU_ASSERT_FALSE(entry.is_collision);
    }
  }

  // Insert one more record, expecting it to work this time
  createBlockName(&name);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, entryCount, name.name,
                                           &entry));
  CU_ASSERT_TRUE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, entryCount,
                                           entryCount & PAYLOAD_MASK, NULL));
  assertKeyValue(&entry, entryCount, entryCount & PAYLOAD_MASK);
  CU_ASSERT_FALSE(entry.is_collision);

  get_delta_index_stats(&di, &stats);
  CU_ASSERT_EQUAL(stats.overflow_count, 1);
  uninitialize_delta_index(&di);
}

/**********************************************************************/
static void lookupTest(void)
{
  struct delta_index di;
  struct delta_index_entry entry, readOnlyEntry;
  enum { PAYLOAD_BITS = 8 };

  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(8, struct uds_record_name,
                                  __func__, &names));

  // Create index with 1 delta list.  Ensure that the saved offset is valid.
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, 1, 256,
                                            PAYLOAD_BITS, 2 * MEGABYTE, 'm'));
  assertSavedValid(&di);

  // Make names for keys 1 to 7.  Insert all but keys 4 and 5 into the index.
  // Ensure that the saved offset is correct after every call.
  unsigned int key;
  for (key = 1; key < 8; key++) {
    createBlockName(&names[key]);
    if ((key < 4) || (key > 5)) {
      UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, key, names[key].name,
                                               &entry));
      CU_ASSERT_TRUE(entry.at_end);
      CU_ASSERT_FALSE(entry.is_collision);
      assertSavedValid(&di);
      assertSavedAt(&entry);
      UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, key, key, NULL));
      assertSavedValid(&di);
      assertSavedAt(&entry);
      assertKeyValue(&entry, key, key);
      CU_ASSERT_FALSE(entry.is_collision);
    }
  }

  // Make 2 collision names, and insert them with key 3.
  // Ensure that the saved offset is correct after every call.
  struct uds_record_name collisions[2];
  unsigned int i;
  for (i = 0; i < 2; i++) {
    createBlockName(&collisions[i]);
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 3, collisions[i].name,
                                             &entry));
    CU_ASSERT_FALSE(entry.at_end);
    CU_ASSERT_FALSE(entry.is_collision);
    assertSavedValid(&di);
    assertSavedAt(&entry);
    UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, 3, i, collisions[i].name));
    assertSavedValid(&di);
    assertSavedBefore(&entry);
    assertKeyValue(&entry, 3, i);
    CU_ASSERT_TRUE(entry.is_collision);
  }

  // Delete a collision.  Between the get and remove calls, insert a
  // read-only get of an earlier record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 3, collisions[0].name,
                                           &entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);
  assertKeyValue(&entry, 3, 0);
  CU_ASSERT_TRUE(entry.is_collision);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 2, names[2].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertKeyValue(&entry, 3, 0);
  CU_ASSERT_TRUE(entry.is_collision);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 2, 2);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);

  // Delete a collision.  Between the get and remove calls, insert a
  // read-only get of a later record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 3, collisions[1].name,
                                           &entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);
  assertKeyValue(&entry, 3, 1);
  CU_ASSERT_TRUE(entry.is_collision);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 6, names[6].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertKeyValue(&entry, 3, 1);
  CU_ASSERT_TRUE(entry.is_collision);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 6, 6);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);

  // Delete a non-collision.  Between the get and remove calls, insert a
  // read-only get of an earlier record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 3, names[3].name, &entry));
  assertSavedValid(&di);
  assertSavedAt(&entry);
  assertKeyValue(&entry, 3, 3);
  CU_ASSERT_FALSE(entry.is_collision);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 2, names[2].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertKeyValue(&entry, 3, 3);
  CU_ASSERT_FALSE(entry.is_collision);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 2, 2);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);

  // Add a non-collision entry.  Between the get and remove calls, insert a
  // read-only get of an earlier record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 4, names[4].name, &entry));
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  assertSavedValid(&di);
  assertSavedAt(&entry);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 2, names[2].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 2, 2);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, 4, 4, NULL));
  assertSavedValid(&di);
  assertSavedBefore(&entry);
  assertKeyValue(&entry, 4, 4);
  CU_ASSERT_FALSE(entry.is_collision);

  // Delete a non-collision.  Between the get and remove calls, insert a
  // read-only get of a later record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 4, names[4].name, &entry));
  assertSavedValid(&di);
  assertSavedAt(&entry);
  assertKeyValue(&entry, 4, 4);
  CU_ASSERT_FALSE(entry.is_collision);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 6, names[6].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertKeyValue(&entry, 4, 4);
  CU_ASSERT_FALSE(entry.is_collision);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 6, 6);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(remove_delta_index_entry(&entry));
  assertSavedValid(&di);
  assertSavedBefore(&entry);

  // Add a non-collision entry.  Between the get and remove calls, insert a
  // read-only get of a later record.  Ensure that the saved offset is
  // correct after every call.
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 5, names[5].name, &entry));
  CU_ASSERT_FALSE(entry.at_end);
  CU_ASSERT_FALSE(entry.is_collision);
  assertSavedValid(&di);
  assertSavedAt(&entry);
  UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, 0, 6, names[6].name,
                                           &readOnlyEntry));
  assertSavedValid(&di);
  assertSavedAt(&readOnlyEntry);
  assertKeyValue(&readOnlyEntry, 6, 6);
  CU_ASSERT_FALSE(readOnlyEntry.is_collision);
  UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, 5, 5, NULL));
  assertSavedValid(&di);
  assertSavedAt(&entry);
  assertKeyValue(&entry, 5, 5);
  CU_ASSERT_FALSE(entry.is_collision);

  uninitialize_delta_index(&di);
  UDS_FREE(names);
}

/**
 * Restore the index for the save/restore test
 **/
static void restoreIndex(struct delta_index *di,
                         struct buffered_reader *bufferedReader)
{
  UDS_ASSERT_SUCCESS(start_restoring_delta_index(di, &bufferedReader, 1));
  UDS_ASSERT_SUCCESS(finish_restoring_delta_index(di, &bufferedReader, 1));
}

/**
 * Verify all the keys for the save/restore test
 **/
static void verifyAllKeys(struct delta_index *di, unsigned int numKeys,
                          unsigned int *keys, unsigned int *lists,
                          struct uds_record_name *names)
{
  struct delta_index_entry entry;
  unsigned int i;
  for (i = 0; i < numKeys; i++) {
    UDS_ASSERT_SUCCESS(get_delta_index_entry(di, lists[i], keys[i],
                                             names[i].name, &entry));
    assertKeyValue(&entry, keys[i], 0);
  }
}

/**********************************************************************/
static void saveRestoreTest(void)
{
  struct delta_index di;
  struct delta_index_entry entry;
  enum { NUM_LISTS = 32 };
  enum { MAX_KEY = 1024 };
  enum { NUM_KEYS = 100 };
  unsigned int meanDelta = (NUM_LISTS * MAX_KEY) / NUM_KEYS;
  enum { MEMORY_SIZE = 2 * MEGABYTE };
  UDS_ASSERT_SUCCESS(initialize_delta_index(&di, ONE_ZONE, NUM_LISTS, meanDelta,
                                            4, MEMORY_SIZE, 'm'));

  // Compute the size needed for saving the delta index
  size_t saveSize = compute_delta_index_save_bytes(NUM_LISTS, MEMORY_SIZE);
  saveSize += sizeof(struct delta_list_save_info);
  saveSize = DIV_ROUND_UP(saveSize, UDS_BLOCK_SIZE);

  // Create the keys+names and put them all into different lists using
  // chapter 0
  unsigned int *keys, *lists;
  struct uds_record_name *names;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_KEYS, unsigned int, __func__, &keys));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_KEYS, unsigned int, __func__, &lists));
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(NUM_KEYS, struct uds_record_name, __func__,
                                  &names));
  unsigned int i;
  for (i = 0; i < NUM_KEYS; i++) {
    keys[i] = random() % MAX_KEY;
    lists[i] = random() % NUM_LISTS;
    createBlockName(&names[i]);
    UDS_ASSERT_SUCCESS(get_delta_index_entry(&di, lists[i], keys[i],
                                             names[i].name, &entry));
    bool isFound = !entry.at_end && entry.key == keys[i];
    UDS_ASSERT_SUCCESS(put_delta_index_entry(&entry, keys[i], 0,
                                             isFound ? names[i].name : NULL));
    assertKeyValue(&entry, keys[i], 0);
  }

  // Verify the data
  verifyAllKeys(&di, NUM_KEYS, keys, lists, names);

  // Do a save, and verify the data
  struct io_factory *factory;
  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));
  struct buffered_writer *writer;
  UDS_ASSERT_SUCCESS(make_buffered_writer(factory, 0, saveSize, &writer));
  UDS_ASSERT_SUCCESS(start_saving_delta_index(&di, 0, writer));
  UDS_ASSERT_SUCCESS(finish_saving_delta_index(&di, 0));
  UDS_ASSERT_SUCCESS(write_guard_delta_list(writer));
  UDS_ASSERT_SUCCESS(flush_buffered_writer(writer));
  free_buffered_writer(writer);
  verifyAllKeys(&di, NUM_KEYS, keys, lists, names);

  // Restore and verify the data
  struct buffered_reader *reader;
  UDS_ASSERT_SUCCESS(make_buffered_reader(factory, 0, saveSize, &reader));
  restoreIndex(&di, reader);
  free_buffered_reader(reader);
  verifyAllKeys(&di, NUM_KEYS, keys, lists, names);

  put_uds_io_factory(factory);
  uninitialize_delta_index(&di);
  UDS_FREE(keys);
  UDS_FREE(lists);
  UDS_FREE(names);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  {"Initialization",         initializationTest },
  {"Basic",                  basicTest },
  {"Record size",            recordSizeTest },
  {"No collisions",          noCollisionsTest },
  {"Collisions",             collisionsTest },
  {"Interleaved Collisions", interleavedTest },
  {"Reversed Collisions",    reversedTest },
  {"Overflow",               overflowTest },
  {"Lookup",                 lookupTest },
  {"Save and Restore",       saveRestoreTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "DeltaIndex_t1",
  .tests = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
