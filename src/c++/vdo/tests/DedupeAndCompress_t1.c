/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/bio.h>
#include <linux/kernel.h>

#include "logger.h"
#include "memory-alloc.h"

#include "packer.h"
#include "slab-depot.h"
#include "vdo.h"

#include "adminUtils.h"
#include "asyncLayer.h"
#include "ioRequest.h"
#include "mutexUtils.h"
#include "packerUtils.h"
#include "slabSummaryUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  IORequest *request;
  char      *buffer;
} ReadRequest;

static const size_t DATA_BLOCKS = 1024 * 5;
static const size_t NUM_RUNS    = 512;

static const size_t WRITE_BATCH      = 4;
static const size_t DEDUPE_BATCH     = 4;
static const size_t OVERWRITE_BATCH  = 2;
static const size_t ZERO_BLOCK_BATCH = 2;
static const size_t READ_BATCH       = 4;

static IORequest **writeRequests;
static size_t      writeRequestCount;
static size_t      writeLaunched = 0;

static ReadRequest *readRequests;
static size_t       readRequestCount;
static size_t       readLaunched = 0;

static struct vdo_slab *slabToSave;
static bool             outputBinsIdle = false;

static atomic64_t ioCount;

/**
 * Test-specific initialization.
 **/
static void initializeDedupeAndCompressT1(void)
{
  const TestParameters parameters = {
    .mappableBlocks      = DATA_BLOCKS * 2,
    .slabJournalBlocks   = 4,
    .journalBlocks       = 1024,
    .logicalThreadCount  = 3,
    .physicalThreadCount = 2,
    .hashZoneThreadCount = 2,
    .compression         = VDO_LZ4,
    .cacheSize           = 64,
  };
  initializeVDOTest(&parameters);

  writeLaunched  = 0;
  readLaunched   = 0;
  outputBinsIdle = false;

  size_t totalWritesPerRun
    = WRITE_BATCH + DEDUPE_BATCH + OVERWRITE_BATCH + ZERO_BLOCK_BATCH;
  writeRequestCount = totalWritesPerRun * NUM_RUNS;
  VDO_ASSERT_SUCCESS(vdo_allocate((writeRequestCount), IORequest *,
                                  "write requests", &writeRequests));

  readRequestCount = 2 * READ_BATCH * NUM_RUNS;
  VDO_ASSERT_SUCCESS(vdo_allocate((readRequestCount), ReadRequest,
                                  "read requests", &readRequests));

  for (size_t i = 0; i < readRequestCount; i++) {
    VDO_ASSERT_SUCCESS(vdo_allocate(VDO_BLOCK_SIZE, char,
                                    "read buffer",
                                    &readRequests[i].buffer));
  }
}

/**
 * Test-specific teardown.
 **/
static void tearDownDedupeAndCompressT1(void)
{
  for (size_t i = 0; i < readRequestCount; i++) {
    free(readRequests[i].buffer);
  }
  free(readRequests);
  free(writeRequests);
  tearDownVDOTest();
}

/**********************************************************************/
static void launchWrite(logical_block_number_t logical, block_count_t offset)
{
  writeRequests[writeLaunched++] = launchIndexedWrite(logical, 1, offset);
}

/**********************************************************************/
static void launchRead(logical_block_number_t logical)
{
  readRequests[readLaunched].request
    = launchBufferBackedRequest(logical, 1, readRequests[readLaunched].buffer,
                                REQ_OP_READ);
  readLaunched++;
}

/**
 * Simulate a VDO crash and restart it as dirty.
 */
static void crashAndRebuildVDO(void)
{
  crashVDO();
  startVDO(VDO_DIRTY);
  waitForRecoveryDone();
}

/**
 * Get a slab journal from a specific slab.
 *
 * @param  slabNumber  the slab number of the slab journal
 **/
static struct slab_journal *getVDOSlabJournal(slab_count_t slabNumber)
{
  return &vdo->depot->slabs[slabNumber]->journal;
}

/**
 * Test vdo with a mix of read and write.
 **/
static void doReadWriteMix(bool success)
{
  size_t writeOffset     = 1;
  size_t overwriteOffset = 0;
  size_t zeroBlockOffset = 0;

  for (size_t iteration = 0; iteration < NUM_RUNS; iteration++) {
    // Batch write data.
    for (size_t batched = 0; batched < WRITE_BATCH; batched++) {
      launchWrite(writeLaunched, writeOffset);
      writeOffset++;
    }

    // Batch read data.
    for (size_t batched = 0; batched < READ_BATCH; batched++) {
      launchRead(readLaunched);
    }

    // Batch write duplicate data.
    for (size_t batched = 0; batched < DEDUPE_BATCH; batched++) {
      launchWrite(writeLaunched, writeOffset - 1);
    }

    // Batch read data.
    for (size_t batched = 0; batched < READ_BATCH; batched++) {
      launchRead(readLaunched);
    }

    // Batch overwrite existing blocks.
    for (size_t batched = 0; batched < OVERWRITE_BATCH; batched++) {
      launchWrite(overwriteOffset, overwriteOffset + 3);
      overwriteOffset++;
    }

    // Batch write zero blocks.
    for (size_t batched = 0; batched < ZERO_BLOCK_BATCH; batched++) {
      launchWrite(zeroBlockOffset * 2, 0);
      zeroBlockOffset++;
    }
  }

  // Wait for all reads to complete.
  for (size_t waiting = 0; waiting < readRequestCount; waiting++) {
    if (readRequests[waiting].request != NULL) {
      int result
        = awaitAndFreeRequest(vdo_forget(readRequests[waiting].request));
      if (success) {
        CU_ASSERT_EQUAL(result, VDO_SUCCESS);
      }
    }
  }

  // Turn off compression to prevent further packing and then flush packer.
  performSetVDOCompressing(false);

  // Wait for all writes to complete.
  for (size_t waiting = 0; waiting < writeRequestCount; waiting++) {
    if (writeRequests[waiting] != NULL) {
      int result
        = awaitAndFreeRequest(vdo_forget(writeRequests[waiting]));
      if (success) {
        CU_ASSERT_EQUAL(result, VDO_SUCCESS);
      }
    }
  }
}



/**
 * Test vdo with a mix of read and write.
 **/
static void testReadWriteMix(void) {
  doReadWriteMix(true);

  struct packer_statistics stats = vdo_get_packer_statistics(vdo->packer);
  CU_ASSERT_EQUAL(0, stats.compressed_fragments_in_packer);

  // Flush slab journals and refCounts. Mark them as dirty in the slab
  // summary to force slab scrubbing.
  performSuccessfulDepotAction(VDO_ADMIN_STATE_RECOVERING);

  struct slab_depot *depot = vdo->depot;
  for (slab_count_t i = 0; i < depot->slab_count; i++) {
    slabToSave = depot->slabs[i];
    performSuccessfulSlabAction(slabToSave, VDO_ADMIN_STATE_SAVE_FOR_SCRUBBING);
    struct slab_journal *slabJournal = getVDOSlabJournal(slabToSave->slab_number);
    tail_block_offset_t tailBlockOffset = slabJournal->last_summarized % slabJournal->size;
    bool loadRefCounts =
      slabToSave->allocator->summary_entries[slabToSave->slab_number].load_ref_counts;
    performSlabSummaryUpdate(slabToSave->slab_number, tailBlockOffset, loadRefCounts, false, 1000);
    struct block_allocator *allocator =
      &depot->allocators[i % vdo->thread_config.physical_zone_count];
    CU_ASSERT(allocator->summary_entries[slabToSave->slab_number].is_dirty);
  }

  crashAndRebuildVDO();
}

/**********************************************************************/
static bool injectIOErrors(struct bio *bio)
{
  if (bio_data_dir(bio) == READ) {
    return true;
  }

  struct vio *vio = bio->bi_private;
  if ((vio != NULL) && (vio->type == VIO_TYPE_SUPER_BLOCK)) {
    return true;
  }

  if (atomic64_read(&ioCount) > 512) {
    bio->bi_status = BLK_STS_VDO_INJECTED;
    bio->bi_end_io(bio);
    return false;
  }

  atomic64_add(1, &ioCount);
  return true;
}

/**
 * Do a mix of reads and writes, with injected I/O errors partway through.
 **/
static void testReadWriteMixWithErrors(void)
{
  atomic64_set(&ioCount, 0);
  setBIOSubmitHook(injectIOErrors);
  doReadWriteMix(false);
  clearBIOSubmitHook();
  setStartStopExpectation(VDO_READ_ONLY);
  stopVDO();
  startVDO(VDO_READ_ONLY_MODE);
}

/**********************************************************************/

static CU_TestInfo vdoTests[] = {
  { "Mixed compressible and dedupe data",         testReadWriteMix           },
  { "Injected I/O errors during mixed workload",  testReadWriteMixWithErrors },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo vdoSuite = {
  .name  = "VDO dedupe and compression tests (DedupeAndCompress_t1)",
  .initializerWithArguments = NULL,
  .initializer              = initializeDedupeAndCompressT1,
  .cleaner                  = tearDownDedupeAndCompressT1,
  .tests                    = vdoTests
};

CU_SuiteInfo *initializeModule(void)
{
  return &vdoSuite;
}
