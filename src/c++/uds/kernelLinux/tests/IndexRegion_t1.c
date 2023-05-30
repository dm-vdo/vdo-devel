// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * This suite includes tests of the Linux I/O region.
 **/

#include <linux/blkdev.h>
#include <linux/fs.h>

#include "albtest.h"
#include "assertions.h"
#include "index-layout.h"
#include "memory-alloc.h"

static const char *indexName;

/**********************************************************************/
static void namesTest(void)
{
  struct uds_parameters params = {
    .memory_size = UDS_MEMORY_CONFIG_256MB,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));

  // Make a layout by using the path (the default)
  struct index_layout *layout;
  config->name = indexName;
  UDS_ASSERT_SUCCESS(uds_make_index_layout(config, true, &layout));
  uds_free_index_layout(UDS_FORGET(layout));

  // Find the device number and make a layout using it
  struct block_device *bdev = blkdev_get_by_path(indexName, FMODE_READ, NULL);
  CU_ASSERT_FALSE(IS_ERR(bdev));

  char *deviceNumber;
  UDS_ASSERT_SUCCESS(uds_alloc_sprintf(__func__, &deviceNumber, "%u:%u",
                                       MAJOR(bdev->bd_dev),
                                       MINOR(bdev->bd_dev)));
  blkdev_put(bdev, FMODE_READ);

  config->name = deviceNumber;
  UDS_ASSERT_SUCCESS(uds_make_index_layout(config, true, &layout));
  uds_free_index_layout(layout);
  UDS_FREE(deviceNumber);
  uds_free_configuration(config);
}

/**********************************************************************/
static void initializerWithIndexName(const char *in)
{
  indexName = in;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"test name specifications", namesTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "IndexRegion_t1",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests,
};

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
