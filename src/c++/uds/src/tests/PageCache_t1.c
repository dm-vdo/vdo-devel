// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "numeric.h"
#include "page-cache.h"
#include "testPrototypes.h"

static struct configuration *config;
static struct page_cache *cache;

/**********************************************************************/
static int getMostRecentPageFromCache(struct page_cache   *cache,
                                      struct cached_page **pagePtr)
{
  if (cache == NULL) {
    return UDS_BAD_STATE;
  }

  uint16_t mostRecentIndex = 0;
  unsigned int i;
  for (i = 0; i < cache->num_cache_entries; i++) {
    if (cache->cache[i].cp_last_used
        >= cache->cache[mostRecentIndex].cp_last_used) {
      mostRecentIndex = i;
    }
  }

  struct cached_page *page = &cache->cache[mostRecentIndex];
  *pagePtr = (((page != NULL)
               && (page->cp_physical_page == cache->num_index_entries))
              ? NULL
              : page);

  return UDS_SUCCESS;
}

/**********************************************************************/
static int getNextMostRecentPageFromCache(struct page_cache   *cache,
                                          struct cached_page  *currentPage,
                                          struct cached_page **pagePtr)
{
  if (cache == NULL) {
    return UDS_BAD_STATE;
  }

  int result = assert_page_in_cache(cache, currentPage);
  if (result != UDS_SUCCESS) {
    return result;
  }

  bool foundNextMostRecentPage = false;
  uint16_t currentIndex = currentPage - cache->cache;
  uint16_t nextMostRecentIndex = 0;
  unsigned int i;
  for (i = 0; i < cache->num_cache_entries; i++) {
    if (i != currentIndex
        && (!foundNextMostRecentPage
            || cache->cache[i].cp_last_used >
               cache->cache[nextMostRecentIndex].cp_last_used)
        && cache->cache[i].cp_last_used < currentPage->cp_last_used) {
      foundNextMostRecentPage = true;
      nextMostRecentIndex = i;
    }
  }

  struct cached_page *page = &cache->cache[nextMostRecentIndex];
  *pagePtr = (!foundNextMostRecentPage
              || ((page != NULL)
                  && (page->cp_physical_page == cache->num_index_entries))
              ? NULL
              : page);

  return UDS_SUCCESS;
}

/**********************************************************************/
static int addPageToCache(struct page_cache *cache, unsigned int physicalPage,
                          struct cached_page **pagePtr)
{
  struct cached_page *page = NULL;
  UDS_ASSERT_SUCCESS(select_victim_in_cache(cache, &page));
  UDS_ASSERT_SUCCESS(put_page_in_cache(cache, physicalPage, page));
  CU_ASSERT_PTR_NOT_NULL(page);

  *pagePtr = page;

  return UDS_SUCCESS;
}

/**********************************************************************/
static void fillCache(void)
{
  // Fill page cache
  unsigned int i;
  for (i = 0; i < cache->num_cache_entries; i++) {
    // Add a page
    struct cached_page *page = NULL;
    UDS_ASSERT_SUCCESS(addPageToCache(cache, i, &page));
  }
}

/**********************************************************************/
static void init(void)
{
  struct uds_parameters params = {
    .memory_size = 1,
  };
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  resizeDenseConfiguration(config, 4 * BYTES_PER_RECORD, 5, 10);

  UDS_ASSERT_SUCCESS(make_page_cache(config->geometry, config->cache_chapters,
                                     config->zone_count, &cache));
}

/**********************************************************************/
static void deinit(void)
{
  free_page_cache(cache);
  free_configuration(config);
}

/**********************************************************************/
static void testAddPages(void)
{
  // Add a Page
  struct cached_page *page = NULL;
  UDS_ASSERT_SUCCESS(addPageToCache(cache, 0, &page));

  // Make sure its the most recent entry after adding
  struct cached_page *entry = NULL;
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE(0 == entry->cp_physical_page);
  CU_ASSERT_TRUE(page == entry);

  // Add to cache limit
  unsigned int i;
  for (i = 1; i < cache->num_cache_entries; i++) {
    // Add a page index
    page = NULL;
    UDS_ASSERT_SUCCESS(addPageToCache(cache, i, &page));
  }

  // Verify cache is from most recent to least recent
  int physicalPage = cache->num_cache_entries - 1;
  unsigned int cacheAccessCount = 0;

  entry = NULL;
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  while (entry != NULL) {
    CU_ASSERT_TRUE((unsigned int)physicalPage == entry->cp_physical_page);
    page = NULL;
    UDS_ASSERT_SUCCESS(get_page_from_cache(cache, physicalPage, &page));
    CU_ASSERT_TRUE(page == entry);
    physicalPage--;
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(cache, entry, &entry));
    cacheAccessCount++;
  }
  CU_ASSERT_TRUE(physicalPage == -1);

  // Add one more to cause least recent to be knocked off
  physicalPage = cache->num_cache_entries;

  page = NULL;
  UDS_ASSERT_SUCCESS(addPageToCache(cache, physicalPage, &page));

  // Verify the least recent entry (page 0) is now out of the cache
  physicalPage = cache->num_cache_entries;

  entry = NULL;
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  while (entry != NULL) {
    CU_ASSERT_TRUE((unsigned int)physicalPage == entry->cp_physical_page);
    page = NULL;
    UDS_ASSERT_SUCCESS(get_page_from_cache(cache, physicalPage, &page));
    CU_ASSERT_TRUE(page == entry);
    physicalPage--;
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(cache, entry, &entry));
    cacheAccessCount++;
  }
  CU_ASSERT_TRUE(physicalPage == 0);
}

/**********************************************************************/
static void testUpdatePages(void)
{
  fillCache();

  // Update the least recent used entry (page 0), then check
  // that it is now the most recent used entry
  struct cached_page *entry = NULL;
  UDS_ASSERT_SUCCESS(get_page_from_cache(cache, 0, &entry));
  CU_ASSERT_PTR_NOT_NULL(entry);
  make_page_most_recent(cache, entry);

  // Make sure its the most recent entry after adding
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE(0 == entry->cp_physical_page);
}

/**********************************************************************/
static void testInvalidatePages(void)
{
  fillCache();

  // Invalidate the most recent used entry, then make sure getFirstPage
  // does not return it.
  int physicalPage = cache->num_cache_entries - 1;
  UDS_ASSERT_SUCCESS(find_invalidate_and_make_least_recent(cache,
                                                           physicalPage,
                                                           true));

  // Make sure its the most recent entry after adding
  struct cached_page *entry = NULL;
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  CU_ASSERT_PTR_NOT_NULL(entry);

  CU_ASSERT_TRUE((unsigned int)physicalPage != entry->cp_physical_page);
}

/**********************************************************************/
static void testInvalidateAll(void)
{
  fillCache();

  // Invalidate chapter 1, telling the cache there are 5 pages per chapter
  UDS_ASSERT_SUCCESS(invalidate_page_cache_for_chapter(cache, 1, 5));

  // Make sure pages 6-10 are invalid
  struct cached_page *entry = NULL;
  UDS_ASSERT_SUCCESS(getMostRecentPageFromCache(cache, &entry));
  while (entry != NULL) {
    CU_ASSERT_FALSE((entry->cp_physical_page >= 6)
                    && (entry->cp_physical_page <= 10));
    UDS_ASSERT_SUCCESS(getNextMostRecentPageFromCache(cache, entry, &entry));
  }
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"AddPages",        testAddPages},
  {"UpdatePages",     testUpdatePages},
  {"InvalidatePages", testInvalidatePages},
  {"InvalidateAll",   testInvalidateAll},
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
