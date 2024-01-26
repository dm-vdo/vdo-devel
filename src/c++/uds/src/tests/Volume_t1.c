// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/random.h>

#include "albtest.h"
#include "assertions.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "random.h"
#include "testPrototypes.h"
#include "volume.h"
#include "volumeUtils.h"

static struct index_layout      *layout;
static struct uds_configuration *config;
static struct index_geometry    *geometry;
static struct volume            *volume;

/**********************************************************************/
static void init(struct block_device *bdev)
{
  // Pages need to be large enough for full header (which is the version
  // string plus the geometry, which is currently 88 bytes.  And also large
  // enough to make the storage device happy.
  struct uds_parameters params = {
    .memory_size = 1,
    .bdev = bdev,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, 4096, 8, 128);
  UDS_ASSERT_SUCCESS(uds_make_index_layout(config, true, &layout));

  UDS_ASSERT_SUCCESS(uds_make_volume(config, layout, &volume));

  geometry = config->geometry;
  makePageArray(geometry->pages_per_volume, geometry->bytes_per_page);
  writeTestVolumeData(volume, geometry);
}

/**********************************************************************/
static void deinit(void)
{
  freePageArray();
  uds_free_volume(volume);
  uds_free_configuration(config);
  uds_free_index_layout(uds_forget(layout));
}

/**********************************************************************/
static void verifyPage(unsigned int chapter, unsigned int page)
{
  u32 physicalPage = map_to_physical_page(geometry, chapter, page);
  const u8 *expected = test_pages[physicalPage];
  u8 *actual;
  // Make sure the page read is synchronous
  UDS_ASSERT_SUCCESS(uds_get_volume_record_page(volume, chapter, page, &actual));
  UDS_ASSERT_EQUAL_BYTES(actual, expected, geometry->bytes_per_page);
}

/**********************************************************************/
static void testSequentialGet(void)
{
  unsigned int chapter, page;
  for (chapter = 0; chapter < geometry->chapters_per_volume; ++chapter) {
    for (page = 0; page < geometry->pages_per_chapter; ++page) {
      verifyPage(chapter, page);
    }
  }
}

/**********************************************************************/
static void testStumblingGet(void)
{
  unsigned int page;
  for (page = 0; page < geometry->pages_per_volume;) {
    unsigned int chapter = page / geometry->pages_per_chapter;
    unsigned int relPage = page % geometry->pages_per_chapter;
    verifyPage(chapter, relPage);
    // back one page 25%, same page 25%, forward one page 50%.
    unsigned int action = random() % 4;
    if (action == 0) {
      if (page > 0) {
        --page;
      }
    } else if (action != 1) {
      ++page;
    }
  }
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"SequentialGet", testSequentialGet},
  {"StumblingGet",  testStumblingGet},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                       = "Volume_t1",
  .initializerWithBlockDevice = init,
  .cleaner                    = deinit,
  .tests                      = tests,
};

// =============================================================================
// Entry point required by the module loader. Return a pointer to the
// const CU_SuiteInfo structure.
// =============================================================================

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
