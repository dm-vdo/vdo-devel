// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "radix-sort.h"
#include "string-utils.h"

/**********************************************************************/
static void assertSorted(const byte *keys[], int count, int length)
{
  int i;
  for (i = 1; i < count; i++) {
    CU_ASSERT_TRUE(memcmp(keys[i - 1], keys[i], length) <= 0);
  }
}

/**********************************************************************/
static void assertOneToOne(const byte *a1[], const byte *a2[], int count)
{
  int i1, i2;
  for (i2 = 0; i2 < count; i2++) {
    bool found = false;
    CU_ASSERT_PTR_NOT_NULL(a2[i2]);
    for (i1 = 0; i1 < count; i1++) {
      if (a1[i1] == a2[i2]) {
        // Null it out so we never find it again.
        a1[i1] = NULL;
        found = true;
        break;
      }
    }
    CU_ASSERT_TRUE(found);
  }
}

/**********************************************************************/
static const byte **sortAndVerify(const byte *keys[], unsigned int count,
                                  unsigned int length)
{
  // Make a copy of the keys we're going to sort.
  byte *bytes;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count * sizeof(keys[0]), byte, "keys",
                                  &bytes));
  memcpy(bytes, keys, count * sizeof(keys[0]));
  const byte **copy = (const byte **) bytes;

  // Sort and check that the keys are sorted.
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(count, &radixSorter));
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, (const byte **) keys, count,
                               length));
  free_radix_sorter(radixSorter);
  assertSorted(keys, count, length);

  // Make sure that every pointer we provided is in the sorted array.
  assertOneToOne(copy, keys, count);

  // Copy the sorted keys back for our caller to use.
  memcpy(copy, keys, count * sizeof(keys[0]));
  return copy;
}

/**********************************************************************/
static void sort(const byte *keys[], unsigned int count, unsigned int length)
{
  const byte **copy = sortAndVerify(keys, count, length);

  // Sort the sorted copy.
  UDS_FREE(sortAndVerify(copy, count, length));

  // Note: since the sort is not stable, we can't actually assert that keys
  // and copy are identical.

  // Make copy into a reverse of keys.
  const byte **reversed = copy;
  unsigned int i;
  for (i = 0; i < count; i++) {
    reversed[i] = keys[count - 1 - i];
  }

  // Sort the reversed array.
  UDS_FREE(sortAndVerify(reversed, count, length));
  UDS_FREE(reversed);
}

/**********************************************************************/
static const byte **makeKeys(unsigned int count)
{
  const byte **keys;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count, const byte *, "split", &keys));
  CU_ASSERT_PTR_NOT_NULL(keys);
  return keys;
}

/**********************************************************************/
static const byte **split(const char *strings, unsigned int count,
                          unsigned int length)
{
  CU_ASSERT_EQUAL(strlen(strings), count * length);
  const byte **keys = makeKeys(count);
  unsigned int i;
  for (i = 0; i < count; i++) {
    keys[i] = (const byte *) &strings[i * length];
  }
  return keys;
}

/**********************************************************************/
static char *join(const byte **keys, int count, int length)
{
  char *strings;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(count * length + 1, char, "join", &strings));
  int i;
  for (i = 0; i < count; i++) {
    memcpy(&strings[i * length], keys[i], length);
  }
  return strings;
}

/**********************************************************************/
static void assertJoined(const char *strings, const byte **keys,
                         unsigned int count, unsigned int length)
{
  char *joined = join(keys, count, length);
  CU_ASSERT_STRING_EQUAL(strings, joined);
  UDS_FREE(joined);
}

/**********************************************************************/
static void testEmpty(void)
{
  const byte *keys[] = { NULL };
  sort(keys, 0, UDS_RECORD_NAME_SIZE);
}

/**********************************************************************/
static void testSingleton(void)
{
  const byte name[3] = "foo";
  const byte *keys[1] = { name };
  sort(keys, 0, UDS_RECORD_NAME_SIZE);
  CU_ASSERT_PTR_EQUAL(name, keys[0]);
}

/**********************************************************************/
static void testIdentical(void)
{
  const byte bart[] = "Science class should not end in tragedy";
  const int length = sizeof(bart);
  const int count = 1000;

  const byte **keys = makeKeys(count);
  int i;
  for (i = 0; i < count; i++) {
    keys[i] = bart;
  }
  assertSorted(keys, count, length);
  sort(keys, count, length);
  UDS_FREE(keys);
}

/**********************************************************************/
static void test(const char *strings, unsigned int length,
                 const char *expected)
{
  unsigned int count = strlen(strings) / length;

  const byte **keys = split(strings, count, length);
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(count, &radixSorter));
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, keys, count, length));
  free_radix_sorter(radixSorter);
  assertJoined(expected, keys, count, length);
  UDS_FREE(keys);
}

/**********************************************************************/
static void testPairs(void)
{
  test("0000", 2, "0000");
  test("0001", 2, "0001");
  test("0010", 2, "0010");
  test("0011", 2, "0011");
  test("0100", 2, "0001");
  test("0101", 2, "0101");
  test("0110", 2, "0110");
  test("0111", 2, "0111");
  test("1000", 2, "0010");
  test("1001", 2, "0110");
  test("1010", 2, "1010");
  test("1011", 2, "1011");
  test("1100", 2, "0011");
  test("1101", 2, "0111");
  test("1110", 2, "1011");
  test("1111", 2, "1111");
}

/**********************************************************************/
static void testZeroLength(void)
{
  const byte **reversed = split("ZZXX", 2, 2);
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(2, &radixSorter));
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, reversed, 2, 0));
  free_radix_sorter(radixSorter);
  assertJoined("ZZXX", reversed, 2, 2);
  UDS_FREE(reversed);
}

/**********************************************************************/
static void testZeroCount(void)
{
  const byte **reversed = split("ZZXX", 2, 2);
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(2, &radixSorter));
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, reversed, 0, 2));
  free_radix_sorter(radixSorter);
  assertJoined("ZZXX", reversed, 2, 2);
  UDS_FREE(reversed);
}

/**********************************************************************/
static void testOneByteKeys(void)
{
  test("x", 1, "x");
  test("ETAOINSHRLDU", 1, "ADEHILNORSTU");
  test("121321432154321654321765432187654321987654321", 1,
       "111111111222222223333333444444555556666777889");
}

/**********************************************************************/
static void testSize(int size)
{
  unsigned short *data;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(size, unsigned short, __func__, &data));
  const byte **keys;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(size, const byte *, __func__, &keys));
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(size, &radixSorter));
  int i;
  for (i = 0; i < size; i++) {
    data[i] = i;
    keys[i] = (byte *) &data[i];
  }
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, keys, size, sizeof(data[0])));
  assertSorted(keys, size, sizeof(data[0]));
  for (i = 0; i < size; i++) {
    data[i] = size - i - 1;
    keys[i] = (byte *) &data[i];
  }
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, keys, size, sizeof(data[0])));
  assertSorted(keys, size, sizeof(data[0]));
  free_radix_sorter(radixSorter);
  UDS_FREE(data);
  UDS_FREE(keys);
}

/**********************************************************************/
static void testBig(void)
{
  testSize(0x10000);
}

/**********************************************************************/
static void testRandom(void)
{
  enum { SIZE = 0x10000 };
  unsigned long *data;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(SIZE, unsigned long, __func__, &data));
  const byte **keys;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(SIZE, const byte *, __func__, &keys));
  struct radix_sorter *radixSorter;
  UDS_ASSERT_SUCCESS(make_radix_sorter(SIZE, &radixSorter));
  int i;
  for (i = 0; i < SIZE; i++) {
    keys[i] = (byte *) &data[i];
  }
  get_random_bytes(data, SIZE * sizeof(data[0]));
  UDS_ASSERT_SUCCESS(radix_sort(radixSorter, keys, SIZE, sizeof(data[0])));
  assertSorted(keys, SIZE, sizeof(data[0]));
  free_radix_sorter(radixSorter);
  UDS_FREE(data);
  UDS_FREE(keys);
}

/**********************************************************************/
static void testLittle(void)
{
  testSize(8);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "no keys",         testEmpty       },
  { "singleton key",   testSingleton   },
  { "identical keys",  testIdentical   },
  { "pairs of keys",   testPairs       },
  { "zero length",     testZeroLength  },
  { "zero count",      testZeroCount   },
  { "one byte keys",   testOneByteKeys },
  { "big data",        testBig         },
  { "random data",     testRandom      },
  { "little data",     testLittle      },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "RadixSort_t1",
  .tests = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
