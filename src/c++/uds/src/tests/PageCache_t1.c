// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "numeric.h"
#include "testPrototypes.h"
#include "volume.h"

static struct uds_configuration *config;
static struct page_cache cache;

/**********************************************************************/
static void assertPageInCache(struct page_cache *cache,
                              struct cached_page *page)
{
  uint16_t page_index = cache->index[page->physical_page];

  CU_ASSERT_TRUE(page->physical_page < cache->indexable_pages);
  CU_ASSERT_TRUE(page_index < cache->cache_slots);
  CU_ASSERT_TRUE(&cache->cache[page_index] == page);
}

/**********************************************************************/
static void getMostRecentPageFromCache(struct page_cache   *cache,
                                       struct cached_page **pagePtr)
{
  u16 i;
  struct cached_page *mostRecent = &cache->cache[0];
  for (i = 0; i < cache->cache_slots; i++) {
    if (cache->cache[i].last_used >= mostRecent->last_used) {
      mostRecent = &cache->cache[i];
    }
  }

  *pagePtr = ((mostRecent->physical_page < cache->indexable_pages)
              ? mostRecent
              : NULL);
}

/**********************************************************************/
static int getNextMostRecentPageFromCache(struct page_cache   *cache,
                                          struct cached_page  *currentPage,
                                          struct cached_page **pagePtr)
{
  assertPageInCache(cache, currentPage);

  bool foundNextMostRecentPage = false;
  u16 currentIndex = currentPage - cache->cache;
  u16 nextMostRecentIndex = 0;
  u16 i;
  for (i = 0; i < cache->cache_slots; i++) {
    if (i != currentIndex
        && (!foundNextMostRecentPage
            || cache->cache[i].last_used > cache->cache[nextMostRecentIndex].last_used)
        && cache->cache[i].last_used < currentPage->last_used) {
      foundNextMostRecentPage = true;
      nextMostRecentIndex = i;
    }
  }

  struct cached_page *page = &cache->cache[nextMostRecentIndex];
  *pagePtr = (!foundNextMostRecentPage
              || ((page != NULL)
                  && (page->physical_page == cache->indexable_pages))
              ? NULL
              : page);

  return UDS_SUCCESS;
}

/**********************************************************************/
static int addPageToCache(struct page_cache *cache, u32 physicalPage,
                          struct cached_page **pagePtr)
{
  struct cached_page *page = select_victim_in_cache(cache);
  UDS_ASSERT_SUCCESS(put_page_in_cache(cache, physicalPage, page));
  CU_ASSERT_PTR_NOT_NULL(page);

  *pagePtr = page;

  return UDS_SUCCESS;
}

/**********************************************************************/
static void fillCache(void)
{
  // Fill page cache
  u16 i;
  for (i = 0; i < cache.cache_slots; i++) {
    // Add a page
    struct cached_page *page = NULL;
    UDS_ASSERT_SUCCESS(addPageToCache(&cache, i, &page));
  }
}

/**********************************************************************/
static void init(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
  };
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  resizeDenseConfiguration(config, 4 * BYTES_PER_RECORD, 5, 10);

  UDS_ASSERT_SUCCESS(initialize_page_cache(&cache, config->geometry, config->cache_chapters,
                                           config->zone_count));
}

/**********************************************************************/
static void deinit(void)
{
  uninitialize_page_cache(&cache);
  uds_free_configuration(config);
}

/**********************************************************************/
static void testAddPages(void)
{
  // Add a Page
  struct cached_page *page = NULL;
  UDS_ASSERT_SUCCESS(addPageToCache(&cache, 0, &page));

  // Make sure it's the most recent entry after adding
  struct cached_page *entry = NULL;
  getMostRecentPageFromCache(&cache, &entry);
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE(0 == entry->physical_page);
  CU_ASSERT_TRUE(page == entry);

  // Add to cache limit
  u16 i;
  for (i = 1; i < cache.cache_slots; i++) {
    page = NULL;
    UDS_ASSERT_SUCCESS(addPageToCache(&cache, i, &page));
  }

  // Verify cache is from most recent to least recent
  u32 physicalPage = cache.cache_slots;

  entry = NULL;
  getMostRecentPageFromCache(&cache, &entry);
  while (entry != NULL) {
    physicalPage--;
    CU_ASSERT_TRUE(physicalPage == entry->physical_page);
    page = NULL;
    get_page_from_cache(&cache, physicalPage, &page);
    CU_ASSERT_TRUE(page == entry);
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(&cache, entry, &entry));
  }
  CU_ASSERT_TRUE(physicalPage == 0);

  // Add one more to cause least recent to be knocked off
  physicalPage = cache.cache_slots;

  page = NULL;
  UDS_ASSERT_SUCCESS(addPageToCache(&cache, physicalPage, &page));

  // Verify the least recent entry (page 0) is now out of the cache
  physicalPage = cache.cache_slots + 1;

  entry = NULL;
  getMostRecentPageFromCache(&cache, &entry);
  while (entry != NULL) {
    physicalPage--;
    CU_ASSERT_TRUE(physicalPage == entry->physical_page);
    page = NULL;
    get_page_from_cache(&cache, physicalPage, &page);
    CU_ASSERT_TRUE(page == entry);
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(&cache, entry, &entry));
  }
  CU_ASSERT_TRUE(physicalPage == 1);
}

/**********************************************************************/
static void testUpdatePages(void)
{
  fillCache();

  // Update the least recent used entry (page 0), then check
  // that it is now the most recent used entry
  struct cached_page *entry = NULL;
  get_page_from_cache(&cache, 0, &entry);
  CU_ASSERT_PTR_NOT_NULL(entry);
  make_page_most_recent(&cache, entry);

  // Make sure its the most recent entry after adding
  getMostRecentPageFromCache(&cache, &entry);
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE(0 == entry->physical_page);
}

/**********************************************************************/
static void testInvalidatePages(void)
{
  fillCache();

  // Invalidate the most recent used entry, then make sure
  // getMostRecentPageFromCache does not return it.
  u32 physicalPage = cache.cache_slots - 1;
  struct cached_page *entry = NULL;

  get_page_from_cache(&cache, physicalPage, &entry);
  assertPageInCache(&cache, entry);
  invalidate_page(&cache, physicalPage);

  getMostRecentPageFromCache(&cache, &entry);
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE(physicalPage != entry->physical_page);

  // Invalidate several pages
  for (u16 i = 7; i < 13; i++)
    invalidate_page(&cache, i);

  getMostRecentPageFromCache(&cache, &entry);
  while (entry != NULL) {
    CU_ASSERT((entry->physical_page < 7) || (entry->physical_page >= 13));
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(&cache, entry, &entry));
  }
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"AddPages",        testAddPages},
  {"UpdatePages",     testUpdatePages},
  {"InvalidatePages", testInvalidatePages},
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name        = "PageCache_t1",
  .initializer = init,
  .cleaner     = deinit,
  .tests       = tests,
};

// =============================================================================
// Entry point required by the module loader. Return a pointer to the
// const CU_SuiteInfo structure.
// =============================================================================

const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
