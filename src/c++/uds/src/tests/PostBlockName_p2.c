// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 *
 * Test the steady state performance of deduping data.  Test using a series of
 * "bands", where by a "band" we measure how many chunks we are reposting from
 * a closed chapter before moving on to a different chapter.
 */

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "indexPerfCommon.h"
#include "oldInterfaces.h"
#include "testPrototypes.h"

static const char *indexName;

/**
 * Full Dedupe pattern: Partition the indexed data into regions, typically
 * chapter size, and have a run of dedupe of length bandSize per chunk
 **/
typedef struct {
  uint64_t dedupeOffset;
  unsigned int dedupeRunLength;
  unsigned int regionSize;
  unsigned int chapterSize;
  unsigned int bandSize;
} DedupeBandState;

static void cbDedupe(enum uds_request_type type __attribute__((unused)),
                     int status,
                     OldCookie cookie __attribute__((unused)),
                     struct uds_record_data *duplicateAddress __attribute__((unused)),
                     struct uds_record_data *canonicalAddress,
                     struct uds_record_name *blockName __attribute__((unused)),
                     void *data __attribute__((unused)))
{
  CU_ASSERT_PTR_NOT_NULL(canonicalAddress);
  UDS_ASSERT_SUCCESS(status);
}

/**
 * Compute the start of the next region to visit
 **/
static void skipToNewRegion(FillState *s)
{
  DedupeBandState *ds = (DedupeBandState *) s->private;
  ds->dedupeRunLength = 0;
  ds->dedupeOffset += ds->regionSize - ds->dedupeOffset % ds->regionSize;
}

static uint64_t dedupeBands(FillState *s)
{
  DedupeBandState *ds = (DedupeBandState *) s->private;
  uint64_t currentVal = ds->dedupeOffset;
  ++ds->dedupeRunLength;
  if (ds->dedupeRunLength >= ds->bandSize) {
    skipToNewRegion(s);
  } else {
    ds->dedupeOffset++;
  }
  if (currentVal >= s->nameCounter) {
    albPrint("currentVal=%llu, nameCounter=%llu",
             (unsigned long long) currentVal,
             (unsigned long long) s->nameCounter);
    CU_ASSERT_TRUE(currentVal < s->nameCounter);
  }
  return currentVal;
}

static uint64_t openChapterDupe(FillState *s)
{
  DedupeBandState *ds = (DedupeBandState *) s->private;
  uint64_t currentVal = ds->dedupeOffset;
  if (++ds->dedupeOffset == s->nameCounter) {
    ds->dedupeOffset -= ds->chapterSize;
  }
  return currentVal;
}

/**********************************************************************/
static void dedupePerfTest(void)
{
  initializeOldInterfaces(2000);

  struct uds_parameters params = {
    .memory_size = 1,
    .name = indexName,
  };
  randomizeUdsNonce(&params);

  struct uds_index_session *indexSession;
  UDS_ASSERT_SUCCESS(uds_create_index_session(&indexSession));
  UDS_ASSERT_SUCCESS(uds_open_index(UDS_CREATE, &params, indexSession));

  FillState state = {
    .nameCounter = 0,
    .private = NULL
  };

  // Fill the index
  unsigned int numBlocksPerChapter = getBlocksPerChapter(indexSession);
  uint64_t numBlocksToWrite = getBlocksPerIndex(indexSession);
  unsigned int numBlocksPerGroup = 1 << 24;
  unsigned int numGroups = (unsigned int) (numBlocksToWrite
                                           / numBlocksPerGroup);
  albPrint("\nFill the index with %uM chunks in %u groups of %uM chunks",
           (unsigned int) (numBlocksToWrite >> 20), numGroups,
           numBlocksPerGroup >> 20);
  fill("Filling", indexSession, numGroups, numBlocksPerGroup, &newData, &state,
       cbStatus);

  unsigned int regionSize = numBlocksPerChapter;
  DedupeBandState dedupeState = {
    .dedupeOffset = state.nameCounter - 100 * regionSize,
    .dedupeRunLength = 0,
    .regionSize = regionSize,
    .chapterSize = numBlocksPerChapter,
    .bandSize = numBlocksPerChapter - 1,
  };
  state.private = &dedupeState;
  unsigned int numIters = 20;
  fill("Warmup", indexSession, 1, numIters * regionSize, &dedupeBands, &state,
       cbDedupe);

  // Test steady state performance of open chapter dedupe
  numBlocksToWrite = 1 << 24;
  albPrint("\nAdd %uM open chapter dupes",
           (unsigned int) (numBlocksToWrite >> 20));
  uint64_t chapterStart = state.nameCounter;
  fill("Open chapter near fill", indexSession, 1, numBlocksPerChapter - 1,
       &newData, &state, cbStatus);
  dedupeState.dedupeOffset = chapterStart;
  fill("Open chapter dedupe", indexSession, 1, numBlocksToWrite,
       &openChapterDupe, &state, cbDedupe);

  // Test the performance of different dedupe band sizes
  dedupeState.dedupeOffset = 10 * numIters * regionSize;
  numIters = 40;
  albPrint("\nAdd bands of dedupe from 1 to 2^18, %u iterations each",
           numIters);
  unsigned int bandSize;
  for (bandSize = 1; bandSize <= regionSize; bandSize <<= 1) {
    dedupeState.bandSize = bandSize;
    char label[32];
    snprintf(label, sizeof(label), "Dedupe band %u", bandSize);
    fill(label, indexSession, 1, numIters * bandSize, &dedupeBands, &state,
         cbDedupe);
    skipToNewRegion(&state);
  }

  UDS_ASSERT_SUCCESS(uds_close_index(indexSession));
  UDS_ASSERT_SUCCESS(uds_destroy_index_session(indexSession));
  uninitializeOldInterfaces();
}

/**********************************************************************/
static void initializerWithIndexName(const char *name)
{
  indexName = name;
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "dedupe performance", dedupePerfTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "PostBlockName_p2",
  .initializerWithIndexName = initializerWithIndexName,
  .tests                    = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
