// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "delta-index.h"
#include "memory-alloc.h"
#include "random.h"
#include "testPrototypes.h"

static const u32 MEAN_DELTA = 4096;
static const u32 NUM_PAYLOAD_BITS = 10;

enum {
  GUARD_BITS = (sizeof(uint64_t) - 1) * BITS_PER_BYTE,
};

/* Read a bit field from an arbitrary bit boundary. */
static inline unsigned int
getField(const u8 *memory, uint64_t offset, int size)
{
	const void *addr = memory + offset / BITS_PER_BYTE;

	return ((get_unaligned_le32(addr) >> (offset % BITS_PER_BYTE)) &
		((1 << size) - 1));
}

/*
 * Compare bits between two fields.
 *
 * @param mem1     The base memory byte address (first field)
 * @param offset1  Bit offset into the memory for the start (first field)
 * @param mem2     The base memory byte address (second field)
 * @param offset2  Bit offset into the memory for the start (second field)
 * @param size     The number of bits in the field
 *
 * @return true if fields are the same, false if different
 */
static bool sameBits(const u8 *mem1, uint64_t offset1, const u8 *mem2,
                     uint64_t offset2, int size)
{
  unsigned int field1;
  unsigned int field2;

  enum { FIELD_BITS = 16 };
  while (size >= FIELD_BITS) {
    field1 = getField(mem1, offset1, FIELD_BITS);
    field2 = getField(mem2, offset2, FIELD_BITS);
    if (field1 != field2) {
      return false;
    }

    offset1 += FIELD_BITS;
    offset2 += FIELD_BITS;
    size -= FIELD_BITS;
  }

  if (size > 0) {
    field1 = getField(mem1, offset1, size);
    field2 = getField(mem2, offset2, size);
    if (field1 != field2) {
      return false;
    }
  }

  return true;
}

/**
 * Test move_bits
 **/
static void moveBitsTest(void)
{
  enum {
    NUM_LENGTHS  = 2 * (sizeof(uint64_t) + sizeof(uint32_t)) * BITS_PER_BYTE
  };
  enum { NUM_OFFSETS = sizeof(uint32_t) * BITS_PER_BYTE };
  enum { MEM_SIZE = (NUM_LENGTHS + 6 * BITS_PER_BYTE - 1) / BITS_PER_BYTE };
  enum { MEM_BITS = MEM_SIZE * BITS_PER_BYTE };
  enum { POST_FIELD_GUARD_BYTES = sizeof(uint64_t) - 1 };
  u8 memory[MEM_SIZE + POST_FIELD_GUARD_BYTES];
  u8 data[MEM_SIZE + POST_FIELD_GUARD_BYTES];
  memset(memory, 0, sizeof(memory));

  u32 offset1, offset2, size;
  for (size = 1; size <= NUM_LENGTHS; size++) {
    for (offset1 = 10; offset1 < 10 + NUM_OFFSETS; offset1++) {
      for (offset2 = 10; offset2 < 10 + NUM_OFFSETS; offset2++) {
        get_random_bytes(data, sizeof(data));
        memcpy(memory, data, sizeof(memory));
        move_bits(memory, offset1, memory, offset2, size);
        CU_ASSERT_TRUE(sameBits(data, offset1, memory, offset2, size));
      }
    }
  }
}

/**
 * Set up a delta list
 *
 * @param pdl       The delta lists
 * @param index     The index of the delta list to set up
 * @param gapSize   Number of bits in the gap before this delta list
 * @param listSize  Number of bits in the delta list bit stream
 **/
static void setupDeltaList(struct delta_list *pdl, int index,
                           unsigned int gapSize, unsigned int listSize)
{
  pdl[index].start = pdl[index - 1].start + pdl[index - 1].size + gapSize;
  pdl[index].size  = listSize;
}

/**
 * Test extend_delta_zone
 *
 * @param pdl           The delta lists
 * @param numLists      The number of delta lists
 * @param initialValue  Value used to initialize the delta memory (0 or 0xFF)
 **/
static void testExtend(struct delta_list *pdl, u32 numLists, int initialValue)
{
  struct delta_index *delta_index;
  u8 *random;
  int initSize = (pdl[numLists + 1].start + pdl[numLists + 1].size) / BITS_PER_BYTE;
  size_t pdlSize = (numLists + 2) * sizeof(struct delta_list);

  // Get some random bits
  uint64_t bitsNeeded = 0;
  u32 i;
  for (i = 1; i <= numLists; i++) {
    bitsNeeded += pdl[i].size;
  }

  // move_bits() can read up to seven bytes beyond the bytes it needs.
  uint64_t bytesNeeded = BITS_TO_BYTES(bitsNeeded + GUARD_BITS);
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(bytesNeeded, u8, __func__, &random));
  get_random_bytes(random, bytesNeeded);

  // Get the delta memory corresponding to the delta lists
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct delta_index, __func__, &delta_index));
  UDS_ASSERT_SUCCESS(uds_initialize_delta_index(delta_index, 1, numLists, MEAN_DELTA,
                                                NUM_PAYLOAD_BITS, initSize, 'm'));
  struct delta_zone *dm = &delta_index->delta_zones[0];
  memset(dm->memory, initialValue, dm->size);
  memcpy(dm->delta_lists, pdl, pdlSize);
  validateDeltaLists(dm);

  // Copy the random bits into the delta lists
  uint64_t randomOffset = 0;
  for (i = 1; i <= numLists; i++) {
    u16 size = dm->delta_lists[i].size;
    move_bits(random, randomOffset, dm->memory, dm->delta_lists[i].start, size);
    randomOffset += size;
  }

  // Balance the delta lists - this will move them around evenly (if
  // possible), but should always leave the delta lists in a usable state.
  UDS_ASSERT_ERROR2(UDS_SUCCESS, UDS_OVERFLOW, extend_delta_zone(dm, 0, 0));
  validateDeltaLists(dm);

  // Verify the random data in the delta lists
  randomOffset = 0;
  for (i = 1; i <= numLists; i++) {
    unsigned int size = dm->delta_lists[i].size;
    CU_ASSERT_TRUE(sameBits(random, randomOffset, dm->memory, dm->delta_lists[i].start, size));
    randomOffset += size;
  }

  uds_uninitialize_delta_index(delta_index);
  uds_free(delta_index);
  uds_free(random);
}

/**
 * Finish delta list setup and run the extend_delta_zone tests
 *
 * @param pdl           The delta lists
 * @param numLists      The number of delta lists
 * @param gapSize       The minimum gap to leave before the guard list
 **/
static void guardAndTest(struct delta_list *pdl, u32 numLists, unsigned int gapSize)
{
  struct delta_list *deltaListsCopy;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(numLists + 2, struct delta_list, __func__, &deltaListsCopy));

  // Set the tail guard list, which ends on a 64K boundary
  uint32_t bitsNeeded = pdl[numLists].start + pdl[numLists].size + gapSize + GUARD_BITS;
  uint32_t increment = 64 * KILOBYTE * BITS_PER_BYTE;
  uint32_t bitsUsed = DIV_ROUND_UP(bitsNeeded, increment) * increment;

  pdl[numLists + 1].start = bitsUsed - GUARD_BITS;
  pdl[numLists + 1].size = GUARD_BITS;

  memcpy(deltaListsCopy, pdl, (numLists + 2) * sizeof(struct delta_list));
  testExtend(deltaListsCopy, numLists, 0x00);

  memcpy(deltaListsCopy, pdl, (numLists + 2) * sizeof(struct delta_list));
  testExtend(deltaListsCopy, numLists, 0xFF);
  uds_free(deltaListsCopy);
}

/**
 * Test with different sized blocks.
 *
 * @param increasing    Use increasing sizes
 **/
static void diffBlocks(bool increasing)
{
  enum {
    NUM_SIZES = 2048,
    LIST_COUNT = NUM_SIZES,
  };
  struct delta_list *deltaLists;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(LIST_COUNT + 2, struct delta_list, __func__,
                                  &deltaLists));

  unsigned int gapSize, i, offset;
  for (gapSize = 0; gapSize < 2 * BITS_PER_BYTE; gapSize++) {
    for (offset = 0; offset < BITS_PER_BYTE; offset++) {
      // Zero the first (guard) delta list
      memset(deltaLists, 0, sizeof(struct delta_list));
      // Set the size of the head guard delta list.  This artifice will let
      // us test each list at each bit offset within the byte stream.
      deltaLists[0].size = offset;
      for (i = 0; i < NUM_SIZES; i++) {
        // Each delta list is one bit longer than the preceding list
        setupDeltaList(deltaLists, i + 1, gapSize,
                       increasing ? i : NUM_SIZES - i);
      }
      deltaLists[0].size = 0;

      guardAndTest(deltaLists, LIST_COUNT, gapSize);
    }
  }
  uds_free(deltaLists);
}

/**
 * Test with blocks that decrease in size
 **/
static void largeToSmallTest(void)
{
  diffBlocks(false);
}

/**
 * Test with blocks that increase in size
 **/
static void smallToLargeTest(void)
{
  diffBlocks(true);
}

/**
 * Test with blocks that are random size
 **/
static void randomTest(void)
{
  enum { LIST_COUNT = 8 * 1024 };
  struct delta_list *deltaLists;
  UDS_ASSERT_SUCCESS(UDS_ALLOCATE(LIST_COUNT + 2, struct delta_list, __func__,
                                  &deltaLists));
  unsigned int i;
  for (i = 1; i <= LIST_COUNT; i++) {
    setupDeltaList(deltaLists, i,
                   random() % (sizeof(uint16_t) * BITS_PER_BYTE + 1),
                   random() % (8 * 1024 + 1));
  }

  guardAndTest(deltaLists, LIST_COUNT,
               random() % (sizeof(uint16_t) * BITS_PER_BYTE + 1));
  uds_free(deltaLists);
}

/**********************************************************************/

static const CU_TestInfo deltaMemoryTests[] = {
  {"Move Bits",    moveBitsTest },
  {"SmallToLarge", smallToLargeTest },
  {"LargeToSmall", largeToSmallTest },
  {"Random",       randomTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo deltaMemorySuite = {
  .name  = "DeltaMemory_t2",
  .tests = deltaMemoryTests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &deltaMemorySuite;
}
