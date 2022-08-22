/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/prandom.h>
#include <stdlib.h>

#include "memory-alloc.h"
#include "permassert.h"
#include "syscalls.h"

#include "block-allocator.h"
#include "block-map.h"
#include "block-map-page.h"
#include "slab-depot.h"
#include "status-codes.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "blockMapUtils.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static block_count_t           logicalBlocks;
static physical_block_number_t pbn;

/**
 * Initialize test data structures.
 **/
static void initializeBlockMapT1(void)
{
  const TestParameters parameters = {
    .logicalBlocks  = 1024,
    .mappableBlocks = 111,
    .slabSize       = 16,
    .cacheSize      = 5,
  };

  initializeVDOTest(&parameters);
  logicalBlocks = getTestConfig().config.logical_blocks;
  initializeBlockMapUtils(logicalBlocks);
}

/**
 * Teardown the test data structures.
 **/
static void teardownBlockMapT1(void)
{
  tearDownBlockMapUtils();
  tearDownVDOTest();
}

/**
 * Format a block map page in memory and verify that the encoding is correct.
 *
 * @param buffer       The buffer which holds the page
 * @param nonce        The VDO nonce
 * @param pbn          The absolute PBN of the page
 * @param initialized  Whether the page should be marked as initialized
 **/
static void checkPageFormatting(void                    *buffer,
                                nonce_t                  nonce,
                                physical_block_number_t  pbn,
                                bool                     initialized)
{
  struct block_map_page *page
    = vdo_format_block_map_page(buffer, nonce, pbn, initialized);
  struct block_map_page_header *header = &page->header;

  // Make sure the arrangement of fields isn't accidentally changed. This is
  // the layout for page version 4.1.
  CU_ASSERT_EQUAL(8, offsetof(struct block_map_page, header));
  CU_ASSERT_EQUAL(0, offsetof(struct block_map_page_header, nonce));
  CU_ASSERT_EQUAL(8, offsetof(struct block_map_page_header, pbn));
  // skip and ignore the unused 8-byte field
  CU_ASSERT_EQUAL(24, offsetof(struct block_map_page_header, initialized));
  // skip and ignore the three unused 1-byte fields
  STATIC_ASSERT_SIZEOF(struct block_map_page_header, 28);

  CU_ASSERT_EQUAL(4, __le32_to_cpu(page->version.major_version));
  CU_ASSERT_EQUAL(1, __le32_to_cpu(page->version.minor_version));
  // The version has no getter; only vdo_validate_block_map_page() checks it.

  CU_ASSERT_EQUAL(nonce, __le64_to_cpu(header->nonce));
  // The nonce has no getter; only vdo_validate_block_map_page() checks it.

  CU_ASSERT_EQUAL(pbn, __le64_to_cpu(header->pbn));
  CU_ASSERT_EQUAL(pbn, vdo_get_block_map_page_pbn(page));

  CU_ASSERT_EQUAL(initialized, header->initialized);
  CU_ASSERT_EQUAL(initialized, vdo_is_block_map_page_initialized(page));

  // While we're here, test all the ways to call
  // vdo_mark_block_map_page_initialized().
  CU_ASSERT_FALSE(vdo_mark_block_map_page_initialized(page, initialized));
  CU_ASSERT_EQUAL(initialized, vdo_is_block_map_page_initialized(page));

  CU_ASSERT_TRUE(vdo_mark_block_map_page_initialized(page, !initialized));
  CU_ASSERT_EQUAL(!initialized, vdo_is_block_map_page_initialized(page));

  CU_ASSERT_FALSE(vdo_mark_block_map_page_initialized(page, !initialized));
  CU_ASSERT_EQUAL(!initialized, vdo_is_block_map_page_initialized(page));

  // The PBN and nonce do not change and have no setters other than format,
  // so there's nothing else that mutates the header that needs checking.

  // Re-format as initialized to make sure that vdo_validate_block_map_page()
  // correctly uses the nonce and PBN.
  vdo_format_block_map_page(buffer, nonce, pbn, true);
  CU_ASSERT_EQUAL(VDO_BLOCK_MAP_PAGE_VALID,
                  vdo_validate_block_map_page(page, nonce, pbn));
  CU_ASSERT_EQUAL(VDO_BLOCK_MAP_PAGE_INVALID,
                  vdo_validate_block_map_page(page, nonce + 1, pbn));
  CU_ASSERT_EQUAL(VDO_BLOCK_MAP_PAGE_BAD,
                  vdo_validate_block_map_page(page, nonce, pbn + 1));
}

/**
 * Test that the fields of block_map_page_header are formatted and accessed in
 * little-endian byte order.
 **/
static void pageHeaderTest(void)
{
  byte buffer[VDO_BLOCK_SIZE];

  // Formatting must zero everything after the header, and with these
  // parameters, the entire header must be zero.
  memset(buffer, 0xFF, sizeof(buffer));
  vdo_format_block_map_page(buffer, 0, 0, false);

  for (size_t i = offsetof(struct block_map_page, header);
       i < sizeof(buffer);
       i++) {
    CU_ASSERT_EQUAL(0, buffer[i]);
  }

  checkPageFormatting(buffer, 0, 0, false);
  checkPageFormatting(buffer, 0, 0, true);
  checkPageFormatting(buffer, 0x1234567890ABCDEF, 0, false);
  checkPageFormatting(buffer, 0, 0x1234567890ABCDEF, false);
}

/**
 * Test packing of physical_block_number_t.
 **/
static void packingTest(void)
{
  CU_ASSERT_EQUAL(8, sizeof(physical_block_number_t));
  CU_ASSERT_EQUAL(5, sizeof(struct block_map_entry));

  enum {
    ARRAY_SIZE = 16,
    PBN_BITS   = 36,
  };
  const uint64_t PBN_MASK = ((1ULL << PBN_BITS) - 1);

  // Check that the endpoints of the range of legal PBNs can be represented by
  // the packed encoding.
  struct block_map_entry minPBN
    = vdo_pack_pbn(0, VDO_MAPPING_STATE_UNCOMPRESSED);
  struct block_map_entry maxPBN
    = vdo_pack_pbn(MAXIMUM_VDO_PHYSICAL_BLOCKS - 1,
                   VDO_MAPPING_STATE_UNCOMPRESSED);
  CU_ASSERT_EQUAL(0, vdo_unpack_block_map_entry(&minPBN).pbn);
  CU_ASSERT_EQUAL(MAXIMUM_VDO_PHYSICAL_BLOCKS - 1,
                  vdo_unpack_block_map_entry(&maxPBN).pbn);

  physical_block_number_t pbn[ARRAY_SIZE];
  prandom_bytes(pbn, sizeof(pbn));

  char buffer[VDO_BLOCK_SIZE];
  struct block_map_page *page
    = vdo_format_block_map_page(buffer, 0xdeadbeef, 3, false);

  // Check uncompressed entries, packed by hand
  for (slot_number_t i = 0; i < ARRAY_SIZE; i++) {
    page->entries[i] = vdo_pack_pbn(pbn[i], VDO_MAPPING_STATE_UNCOMPRESSED);
    struct data_location mapping
      = vdo_unpack_block_map_entry(&page->entries[i]);
    CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, mapping.state);
    CU_ASSERT_EQUAL((pbn[i] & PBN_MASK), mapping.pbn);
  }

  // Check uncompressed entries.
  vdo_format_block_map_page(page, 0xdeadbeef, 3, false);
  for (slot_number_t i = 0; i < ARRAY_SIZE; i++) {
    page->entries[i] = vdo_pack_pbn(pbn[i], VDO_MAPPING_STATE_UNCOMPRESSED);
    struct data_location mapping
      = vdo_unpack_block_map_entry(&page->entries[i]);
    CU_ASSERT_EQUAL(VDO_MAPPING_STATE_UNCOMPRESSED, mapping.state);
    CU_ASSERT_EQUAL((pbn[i] & PBN_MASK), mapping.pbn);
  }

  // Now check compressed entries.
  vdo_format_block_map_page(page, 0xdeadbeef, 3, false);
  for (slot_number_t i = 0; i < ARRAY_SIZE; i++) {
    enum block_mapping_state state = ((i < VDO_MAX_COMPRESSION_SLOTS)
                                      ? VDO_MAPPING_STATE_COMPRESSED_BASE + i
                                      : VDO_MAPPING_STATE_UNCOMPRESSED);
    page->entries[i] = vdo_pack_pbn(pbn[i], state);
    struct data_location mapping
      = vdo_unpack_block_map_entry(&page->entries[i]);
    CU_ASSERT_EQUAL(state, mapping.state);
    CU_ASSERT_EQUAL((pbn[i] & PBN_MASK), mapping.pbn);
  }

  // Spot-check that the encoding is in little-endian layout, using
  // a known encoding of PBN and mapping state with distinct nibbles.
  physical_block_number_t distinctPBN = 0xABCDE6789;
  enum block_mapping_state distinctState = (enum block_mapping_state) 0xF;
  byte expectedPacking[] = { 0xAF, 0x89, 0x67, 0xDE, 0xBC };

  struct block_map_entry packed = vdo_pack_pbn(distinctPBN, distinctState);
  UDS_ASSERT_EQUAL_BYTES(expectedPacking, (byte *) &packed, sizeof(packed));

  struct data_location unpacked = vdo_unpack_block_map_entry(&packed);
  CU_ASSERT_EQUAL(distinctPBN, unpacked.pbn);
  CU_ASSERT_EQUAL(distinctState, unpacked.state);
}

/**
 * Implements PopulateBlockMapConfigurator.
 **/
static void configureBasic(struct data_vio *dataVIO)
{
  if (dataVIO->logical.lbn >= logicalBlocks) {
    return;
  }

  while (!vdo_is_physical_data_block(vdo->depot, pbn)) {
    pbn++;
  }

  setBlockMapping(dataVIO->logical.lbn, pbn, VDO_MAPPING_STATE_UNCOMPRESSED);
  dataVIO->new_mapped.pbn   = pbn++;
  dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
}

/**
 * Basic test for BlockMap.
 **/
static void basicTest(void)
{
  // Test an empty map.
  verifyBlockMapping(0);

  // Try making and reading an entry which is out of range.
  char buffer[VDO_BLOCK_SIZE] = { 0, };
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE, performWrite(logicalBlocks, 1, buffer));
  CU_ASSERT_EQUAL(VDO_OUT_OF_RANGE, performRead(logicalBlocks, 1, NULL));

  // Populate the map with some odd-numbered blocks.
  pbn = 1;
  for (logical_block_number_t lbn = 1; lbn <= 10; lbn += 2) {
    populateBlockMap(lbn, 1, configureBasic);
  }

  verifyBlockMapping(0);
}

/**
 * Implements PopulateBlockMapConfigurator.
 **/
static void configureNonce(struct data_vio *dataVIO)
{
  block_count_t physicalBlocks = getTestConfig().config.physical_blocks;
  while (!vdo_is_physical_data_block(vdo->depot, pbn)) {
    pbn++;
    if (pbn >= physicalBlocks) {
      pbn = 1;
    }
  }

  dataVIO->new_mapped.pbn   = pbn++;
  dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
}

/**********************************************************************/
static void nonceTest(void)
{
  verifyBlockMapping(0);
  pbn = 1;
  // Populate with a configurator that will not set any expectations so that
  // when we verify after the reformat, we will expect no entries.
  populateBlockMap(0, logicalBlocks, configureNonce);

  // Now make a new block map with a different nonce.
  restartVDO(true);
  verifyBlockMapping(0);
}

/**
 * Implements PopulateBlockMapConfigurator.
 **/
static void configureInvalid(struct data_vio *dataVIO)
{
  switch (dataVIO->logical.lbn) {
  case 1:
    dataVIO->new_mapped.pbn   = 1;
    dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
    break;

  case 2:
    dataVIO->new_mapped.pbn   = VDO_ZERO_BLOCK;
    dataVIO->new_mapped.state = VDO_MAPPING_STATE_COMPRESSED_MAX;
    break;

  case 3:
    dataVIO->new_mapped.pbn   = pbn;
    dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNMAPPED;
    break;

  default:
    CU_FAIL("Unknown lbn");
  }

  setBlockMappingError(dataVIO->logical.lbn, VDO_BAD_MAPPING);
}

/**********************************************************************/
static void invalidEntryTest(void)
{
  pbn = 1;
  while (!vdo_is_physical_data_block(vdo->depot, pbn)) {
    pbn++;
  }

  populateBlockMap(1, 3, configureInvalid);
  verifyBlockMapping(1);

  populateBlockMap(1, 3, configureBasic);
  verifyBlockMapping(1);
}

/**********************************************************************/
static CU_TestInfo blockMapTests[] = {
  { "page header",      pageHeaderTest   },
  { "packing",          packingTest      },
  { "basic",            basicTest        },
  { "nonce",            nonceTest        },
  { "invalid entries",  invalidEntryTest },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo blockMapSuite = {
  .name                     = "Trivial blockMap tests (BlockMap_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeBlockMapT1,
  .cleaner                  = teardownBlockMapT1,
  .tests                    = blockMapTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &blockMapSuite;
}
