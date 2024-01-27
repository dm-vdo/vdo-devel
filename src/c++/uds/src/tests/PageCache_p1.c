// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "config.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "testPrototypes.h"
#include "volume.h"

typedef struct {
  unsigned long counter;        // only used in testMixed
  unsigned int  threadNum;
  unsigned int  totalThreads;
  unsigned int  percentageHits; // only used in testMixed
} ThreadArg;

enum { MAX_THREADS = 16 };

static ThreadArg args[MAX_THREADS];
static struct thread *threads[MAX_THREADS];

static struct uds_configuration *config;
static struct page_cache cache;
static const unsigned int LOTS = 10000000;
static unsigned long globalCounter;

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
static void fillCacheWithPages(void)
{
  unsigned int i;
  for (i = 1; i < cache.cache_slots; i++) {
    struct cached_page *page = select_victim_in_cache(&cache);
    UDS_ASSERT_SUCCESS(put_page_in_cache(&cache, i, page));
    CU_ASSERT_PTR_NOT_NULL(page);
  }
}

/**********************************************************************/
static void report(ktime_t elapsedTime, int numProbes)
{
  char *elapsed;
  UDS_ASSERT_SUCCESS(rel_time_to_string(&elapsed, elapsedTime));
  albPrint("elapsed time %s for %d probes", elapsed, numProbes);
  uds_free(elapsed);
}

/**********************************************************************/
static void testOptimalGuts(void *arg)
{
  ThreadArg *a = (ThreadArg *) arg;
  struct cached_page *page = NULL;
  u16 physicalPage = cache.cache_slots - 1;
  unsigned int i;
  for (i = 0; i < (LOTS / a->totalThreads); ++i) {
    get_page_from_cache(&cache, physicalPage, &page);
  }
}

/**********************************************************************/
static void testOptimal(void)
{
  albPrint("Optimal case: 100%% cache hits without update");
  init();
  fillCacheWithPages();

  ThreadArg arg;
  arg.counter = 0;
  arg.threadNum = 0;
  arg.totalThreads = 1;
  arg.percentageHits = 100;
  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  testOptimalGuts(&arg);
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void testOptimalMT(unsigned int numThreads)
{
  albPrint("Optimal case: 100%% cache hits without update, %u threads",
           numThreads);
  init();
  fillCacheWithPages();

  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  unsigned int i;
  for (i = 0; i < numThreads; ++i) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                         "tester%d", i));
    args[i].counter = 0;
    args[i].threadNum = i;
    args[i].totalThreads = numThreads;
    args[i].percentageHits = 100;

    UDS_ASSERT_SUCCESS(uds_create_thread(testOptimalGuts, &args[i], nameBuf,
                                         &threads[i]));
  }
  for (i = 0; i < numThreads; ++i) {
    uds_join_threads(threads[i]);
  }
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void testLRUOnlyGuts(void *arg)
{
  ThreadArg *a = (ThreadArg *) arg;
  u16 physicalPage = 1;
  struct cached_page *entry = NULL;

  unsigned int i;
  for (i = 0; i < (LOTS / a->totalThreads); ++i) {
    get_page_from_cache(&cache, physicalPage, &entry);

    make_page_most_recent(&cache, entry);
    if (++physicalPage >= cache.cache_slots) {
      physicalPage = 1;
    }
  }
}

/**********************************************************************/
static void testLRUOnly(void)
{
  albPrint("Update only: 100%% cache hits with update");
  init();
  fillCacheWithPages();

  ThreadArg arg;
  arg.counter = 0;
  arg.threadNum = 0;
  arg.totalThreads = 1;
  arg.percentageHits = 100;
  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  testLRUOnlyGuts(&arg);
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void testLRUOnlyMT(unsigned int numThreads)
{
  albPrint("Update only: 100%% cache hits with update, %u threads", numThreads);
  init();
  fillCacheWithPages();

  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  unsigned int i;
  for (i = 0; i < numThreads; ++i) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                         "tester%d", i));
    args[i].counter = 0;
    args[i].threadNum = i;
    args[i].totalThreads = numThreads;
    args[i].percentageHits = 100;
    UDS_ASSERT_SUCCESS(uds_create_thread(testLRUOnlyGuts, &args[i], nameBuf,
                                         &threads[i]));
  }
  for (i = 0; i < numThreads; ++i) {
    uds_join_threads(threads[i]);
  }
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void testMixedGuts(void *arg)
{
  ThreadArg *a = (ThreadArg *) arg;
  u16 physicalPage = 1;
  u16 absentPage = cache.cache_slots + 1;
  struct cached_page *entry = NULL;
  unsigned int i;

  for (i = 0; i < (LOTS / a->totalThreads); ++i) {
    union {
      unsigned char hash[ 128 / 8 ];
      unsigned int val;
    } rand;

    murmurhash3_128(&a->counter, sizeof(a->counter), a->threadNum, &rand.hash);
    a->counter += 1;
    if (rand.val % 100 < a->percentageHits) {
      get_page_from_cache(&cache, physicalPage, &entry);
      make_page_most_recent(&cache, entry);
    } else {
      get_page_from_cache(&cache, absentPage, &entry);
    }
    if (++physicalPage >= cache.cache_slots) {
      physicalPage = 1;
    }
    if (++absentPage >= cache.indexable_pages) {
      absentPage = cache.cache_slots + 1;
    }
  }
}

/**********************************************************************/
static void testMixed(int percentageHits)
{
  albPrint("%d%% cache hits with update on hit", percentageHits);
  init();
  fillCacheWithPages();

  ThreadArg arg;
  arg.counter = globalCounter;
  arg.threadNum = 0;
  arg.totalThreads = 1;
  arg.percentageHits = percentageHits;
  globalCounter += LOTS;
  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  testMixedGuts(&arg);
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void testMixedMT(unsigned int numThreads, int percentageHits)
{
  albPrint("%d%% cache hits with update on hit, %u threads", percentageHits,
           numThreads);
  init();
  fillCacheWithPages();

  ktime_t loopStart = current_time_ns(CLOCK_MONOTONIC);
  unsigned int i;
  for (i = 0; i < numThreads; ++i) {
    char nameBuf[100];
    UDS_ASSERT_SUCCESS(uds_fixed_sprintf(nameBuf, sizeof(nameBuf),
                                         "tester%d", i));
    args[i].counter = globalCounter;
    args[i].threadNum = i;
    args[i].totalThreads = numThreads;
    args[i].percentageHits = percentageHits;
    globalCounter += LOTS;
    UDS_ASSERT_SUCCESS(uds_create_thread(testMixedGuts, &args[i], nameBuf,
                                         &threads[i]));
  }
  for (i = 0; i < numThreads; ++i) {
    uds_join_threads(threads[i]);
  }
  ktime_t loopElapsed = ktime_sub(current_time_ns(CLOCK_MONOTONIC), loopStart);
  report(loopElapsed, LOTS);

  deinit();
}

/**********************************************************************/
static void singleThreadTest(void)
{
  testOptimal();
  testLRUOnly();
  testMixed(75);
  testMixed(50);
  testMixed(25);
  testMixed(0);
}

/**********************************************************************/
static void multipleThreadTest(void)
{
  const unsigned int numThreads = 4;
  CU_ASSERT_TRUE(numThreads <= MAX_THREADS);
  testOptimalMT(numThreads);
  testLRUOnlyMT(numThreads);
  testMixedMT(numThreads, 75);
  testMixedMT(numThreads, 50);
  testMixedMT(numThreads, 25);
  testMixedMT(numThreads, 0);
}

static const CU_TestInfo tests[] = {
  { "single thread",   singleThreadTest },
  { "multiple thread", multipleThreadTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "PageCache_p1",
  .tests = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
