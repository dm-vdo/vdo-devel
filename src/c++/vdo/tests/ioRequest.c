/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "ioRequest.h"

#include <linux/bio.h>

#include "memory-alloc.h"
#include "numeric.h"

#include "data-vio.h"

#include "dataBlocks.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"

enum {
  BLOCKS_PER_MB = 1024 * 1024 / VDO_BLOCK_SIZE,
  DEFAULT_MAX_DISCARD_SIZE = 8,
};

/**
 * A function to generate the data buffer for a write. Each call to the
 * generator will return one block.
 *
 * @param context  The context for the generator
 * @param start    The starting sector for the buffer
 * @param sectors  The number of sectors spanned by the buffer
 *
 * @return The next block in the generator's sequence
 **/
typedef struct page *DataGenerator(void *context,
                                   sector_t start,
                                   sector_t sectors);

/**********************************************************************/
void freeRequest(IORequest *request)
{
  if (request == NULL) {
    return;
  }

  BIO *bio = uds_forget(request->bios);
  while (bio != NULL) {
    BIO *toFree = bio;
    bio = bio->next;
    vdo_free_bio(uds_forget(toFree->bio));
    uds_free(toFree);
  }

  uds_free(request);
}

/**
 * Launch an IORequest.
 *
 * @param request  The request to launch
 *
 * @return  The request (for the convenience of callers)
 **/
static IORequest *launchIORequest(IORequest *request)
{
  for (BIO *bio = request->bios; bio != NULL; ) {
    BIO *toLaunch = bio;
    bio = bio->next;
    vdo_launch_bio(vdo->data_vio_pool, toLaunch->bio);
  }

  return request;
}

/**
 * Check whether an IORequest is complete.
 *
 * Implements WaitCondition
 **/
static bool isRequestComplete(void *context)
{
  IORequest *request = context;
  return ((request->completeCount == request->bioCount)
          && (request->acknowledgedCount == request->bioCount));
}

/**
 * Account for the completion of a data vio while holding the mutex.
 *
 * Implements LockedMethod.
 **/
static bool dataVIOReleased(void *context)
{
  struct vdo_completion *completion = context;
  IORequest *request = ((BIO *) uds_forget(completion->parent))->request;
  if (request->result == VDO_SUCCESS) {
    request->result = completion->result;
  }

  CU_ASSERT(request->completeCount < request->bioCount);
  request->completeCount++;
  return isRequestComplete(request);
}

/**********************************************************************/
void release_data_vio_hook(struct data_vio *data_vio)
{
  runLocked(dataVIOReleased, &data_vio->vio.completion);
}

/**********************************************************************/
int awaitRequest(IORequest *request)
{
  waitForCondition(isRequestComplete, request);
  return request->result;
}

/**
 * Account for the acknowledgement of a DataBIO while holding the lock.
 *
 * Implements LockedMethod.
 **/
static bool noteAcknowledgementLocked(void *context)
{
  BIO *bio = context;
  CU_ASSERT_FALSE(bio->acknowledged);
  bio->acknowledged = true;

  IORequest *request = bio->request;
  CU_ASSERT(request->acknowledgedCount < request->bioCount);
  request->acknowledgedCount++;
  return isRequestComplete(request);
}

/**
 * bio_end_io callback for the user bio which notes that the bio has been
 * acknowledged, and asserts that it has not been acknowledged more than once.
 **/
static void noteAcknowledgement(struct bio *bio)
{
  runLocked(noteAcknowledgementLocked, bio->unitTestContext);
}

/**
 * Compute the sector of the start of the next bio given the start of the
 * current bio.
 *
 * @param sector            The start of the current bio
 * @param end               The end of the IORequest
 * @param maxSectorsPerBIO  The maximum number of sectors in a single bio
 *
 * @return The start of the next sector (i.e. the end of the current sector)
 **/
static inline sector_t nextBIOSector(sector_t sector,
                                     sector_t end,
                                     size_t   maxSectorsPerBIO)
{
  CU_ASSERT(maxSectorsPerBIO >= VDO_SECTORS_PER_BLOCK);

  sector_t partial = sector % VDO_SECTORS_PER_BLOCK;
  if (partial > 0) {
    return sector + min(VDO_SECTORS_PER_BLOCK - partial, end - sector);
  }

  return sector + min(maxSectorsPerBIO, end - sector);
}

/**
 * Allocate an IORequest.
 *
 * @param start             The sector at which the request begins
 * @param sectors           The number of sectors in the request
 * @param maxSectorsPerBIO  The maximum size of each bio, should be 8 for
 *                          data, and up to 64 for discards
 * @param generator         The generator for the data backing the request
 * @param generatorContext  The context for the data generator
 * @param operation         The operation for the request to perform
 *
 * @return The request
 **/
__attribute__((warn_unused_result))
static IORequest *
allocateIORequest(logical_block_number_t  start,
                  block_count_t           count,
                  size_t                  maxSectorsPerBIO,
                  DataGenerator          *generator,
                  void                   *generatorContext,
                  uint32_t                operation)
{
  IORequest *request;
  VDO_ASSERT_SUCCESS(uds_allocate(1, IORequest, __func__, &request));

  sector_t end = start + count;
  sector_t nextSector;
  BIO **tail = &request->bios;
  for (sector_t sector = start; sector < end; sector = nextSector) {
    nextSector = nextBIOSector(sector, end, maxSectorsPerBIO);
    sector_t length = nextSector - sector;
    uint32_t size   = length * VDO_SECTOR_SIZE;
    VDO_ASSERT_SUCCESS(uds_allocate(1, BIO, __func__, tail));
    BIO *bio = *tail;
    VDO_ASSERT_SUCCESS(vdo_create_bio(&bio->bio));
    *bio->bio = (struct bio) {
      .unitTestContext = bio,
      .bi_opf          = operation,
      .bi_iter         = (struct bvec_iter) {
        .bi_sector  = sector,
        .bi_size    = size,
        .bi_idx     = 0,
      },
      .bi_end_io       = noteAcknowledgement,
      .bi_vcnt         = 1,
      .bi_max_vecs     = 1,
      .bi_io_vec       = bio->bio->bi_inline_vecs,
    };

    bio->bio->bi_inline_vecs[0] = (struct bio_vec) {
      .bv_page = generator(generatorContext, sector, length),
      .bv_len  = size,
    };

    bio->request = request;

    tail = &bio->next;
    request->bioCount++;
  }

  return request;
}

/**
 * Generate no data.
 *
 * Implements DataGenerator.
 **/
static struct page *generateNULL(void *context __attribute__((unused)),
                                 sector_t start __attribute__((unused)),
                                 sector_t sectors __attribute__((unused)))

{
  return NULL;
}

/**
 * Generate request data from a supplied buffer.
 *
 * <p>Implements DataGenerator.
 **/
static struct page *generateFromBuffer(void *context,
                                       sector_t start __attribute__((unused)),
                                       sector_t sectors)
{
  char *data = *((char **) context);
  *((char **) context) = data + (sectors * VDO_SECTOR_SIZE);
  return (struct page *) data;
}

/**
 * Create an IORequest backed by a supplied buffer.
 *
 * @param  start      The sector at which the request's operation starts
 * @param  count      The number of sectors in the request
 * @param  buffer     The buffer backing the request
 * @param  operation  The operation the request is to perform
 *
 * @return The request
 **/
 __attribute__((warn_unused_result))
static IORequest *createRequestFromBuffer(sector_t  start,
                                          sector_t  count,
                                          char     *buffer,
                                          uint32_t  operation)
{
  DataGenerator *generator = ((buffer == NULL)
                              ? generateNULL
                              : generateFromBuffer);
  return allocateIORequest(start,
                           count,
                           VDO_SECTORS_PER_BLOCK,
                           generator,
                           &buffer,
                           operation);
}

/**********************************************************************/
IORequest *launchUnalignedBufferBackedRequest(sector_t  start,
                                              sector_t  count,
                                              char     *buffer,
                                              uint32_t  operation)
{
  return launchIORequest(createRequestFromBuffer(start,
                                                 count,
                                                 buffer,
                                                 operation));
}

/**********************************************************************/
IORequest *launchBufferBackedRequest(logical_block_number_t  start,
                                     block_count_t           count,
                                     char                   *buffer,
                                     uint32_t                operation)
{
  return launchUnalignedBufferBackedRequest(start * VDO_SECTORS_PER_BLOCK,
                                            count * VDO_SECTORS_PER_BLOCK,
                                            buffer,
                                            operation);
}

/**
 * Auto-generate request data.
 *
 * Implements DataGenerator.
 **/
static struct page *autoGenerate(void *context,
                                 sector_t start __attribute__((unused)),
                                 sector_t sectors __attribute__((unused)))
{
  block_count_t index = *((block_count_t *) context);
  *((block_count_t *) context) = index + 1;
  return (struct page *) getDataBlock(index);
}

/**
 * Create an IORequest for writing with auto-generated test data.
 *
 * @param start  The logical block at which the request's operation starts
 * @param count  The number of blocks in the request
 * @param index  The index for generating the data to write
 *
 * @return The request
 **/
static IORequest * __must_check
createIndexedWrite(logical_block_number_t start,
                   block_count_t          count,
                   block_count_t          index)
{
  CU_ASSERT_TRUE(count <= MAXIMUM_VDO_USER_VIOS);
  return allocateIORequest(start * VDO_SECTORS_PER_BLOCK,
                           count * VDO_SECTORS_PER_BLOCK,
                           VDO_SECTORS_PER_BLOCK,
                           autoGenerate,
                           &index,
                           REQ_OP_WRITE);
}

/**********************************************************************/
IORequest *launchIndexedWrite(logical_block_number_t start,
                              block_count_t          count,
                              block_count_t          index)
{
  return launchIORequest(createIndexedWrite(start, count, index));
}

/**
 * Create a trim request.
 *
 * @param start        The logical block at which the request's operation starts
 * @param count        The number of blocks in the request
 * @param discardSize  The maximum number of discard blocks per bio (hence
 *                     data_vio)
 *
 * @return The request
 **/
static IORequest * __must_check createTrim(logical_block_number_t start,
                                           block_count_t count,
                                           block_count_t discardSize)
{
  return allocateIORequest(start * VDO_SECTORS_PER_BLOCK,
                           count * VDO_SECTORS_PER_BLOCK,
                           discardSize * VDO_SECTORS_PER_BLOCK,
                           generateNULL,
                           NULL,
                           REQ_OP_DISCARD);
}

/**********************************************************************/
IORequest *launchTrimWithMaxDiscardSize(logical_block_number_t start,
                                        block_count_t          count,
                                        block_count_t          size)
{
  return launchIORequest(createTrim(start, count, size));
}

/**********************************************************************/
IORequest *launchTrim(logical_block_number_t start, block_count_t count)
{
  return launchTrimWithMaxDiscardSize(start, count, DEFAULT_MAX_DISCARD_SIZE);
}

/**********************************************************************/
int performRead(logical_block_number_t  start,
                block_count_t           count,
                char                   *buffer)
{
  return awaitAndFreeRequest(launchBufferBackedRequest(start,
                                                       count,
                                                       buffer,
                                                       REQ_OP_READ));
}

/**********************************************************************/
int performWrite(logical_block_number_t  start,
                 block_count_t           count,
                 char                   *buffer)
{
  return awaitAndFreeRequest(launchBufferBackedRequest(start,
                                                       count,
                                                       buffer,
                                                       REQ_OP_WRITE));
}

/**********************************************************************/
int performIndexedWrite(logical_block_number_t start,
                        block_count_t          count,
                        block_count_t          index)
{
  return awaitAndFreeRequest(launchIndexedWrite(start, count, index));
}

/**********************************************************************/
void writeData(logical_block_number_t start,
               block_count_t          index,
               block_count_t          count,
               int                    expectedResult)
{
  CU_ASSERT_EQUAL(performIndexedWrite(start, count, index), expectedResult);
}

/**********************************************************************/
void zeroData(logical_block_number_t startBlock,
              block_count_t          blockCount,
              int                    expectedResult)
{
  // uds_allocate always returns zeroed data
  char *buffer;
  UDS_ASSERT_SUCCESS(uds_allocate(blockCount * VDO_BLOCK_SIZE, char,
                                  "test buffer", &buffer));

  CU_ASSERT_EQUAL(performWrite(startBlock, blockCount, buffer),
                  expectedResult);
  uds_free(buffer);
}

/**********************************************************************/
void verifyData(logical_block_number_t startBlock,
                block_count_t          blockOffset,
                block_count_t          count)
{
  // Don't blow the stack
  char buffer[VDO_BLOCK_SIZE * BLOCKS_PER_MB];
  for (block_count_t i = 0; i < count; i += BLOCKS_PER_MB) {
    block_count_t blocks = min((block_count_t) BLOCKS_PER_MB, count - i);
    VDO_ASSERT_SUCCESS(performRead(startBlock + i, blocks, buffer));
    // Check the blocks one at a time to make finding the first error easier.
    for (block_count_t j = 0; j < blocks; j++) {
      UDS_ASSERT_EQUAL_BYTES(getDataBlock(blockOffset + i + j),
                             buffer + (VDO_BLOCK_SIZE * j),
                             VDO_BLOCK_SIZE);
    }
  }
}

/**********************************************************************/
void verifyZeros(logical_block_number_t startBlock, block_count_t count)
{
  // Don't blow the stack
  char buffer[VDO_BLOCK_SIZE * BLOCKS_PER_MB];
  for (block_count_t i = 0; i < count; i += BLOCKS_PER_MB) {
    block_count_t blocks = min((block_count_t) BLOCKS_PER_MB, count - i);
    VDO_ASSERT_SUCCESS(performRead(startBlock + i, blocks, buffer));
    for (size_t j = 0; j < blocks * VDO_BLOCK_SIZE; j++) {
      CU_ASSERT_EQUAL(0, buffer[j]);
    }
  }
}

/**
 * Check that the blocks free and blocks allocated stats are as expected.
 *
 * @param expectedBlocksFree       The expected number of free blocks
 * @param expectedBlocksAllocated  The expected number of allocated blocks
 **/
static void checkStats(block_count_t expectedBlocksFree,
                       block_count_t expectedBlocksAllocated)
{
  CU_ASSERT_EQUAL(expectedBlocksFree, getPhysicalBlocksFree());
  CU_ASSERT_EQUAL(expectedBlocksAllocated,
                  vdo_get_physical_blocks_allocated(vdo));
}

/**********************************************************************/
void verifyWrite(logical_block_number_t startBlock,
                 block_count_t          blockOffset,
                 block_count_t          blockCount,
                 block_count_t          expectedBlocksFree,
                 block_count_t          expectedBlocksAllocated)
{
  verifyData(startBlock, blockOffset, blockCount);
  checkStats(expectedBlocksFree, expectedBlocksAllocated);
}

/**********************************************************************/
void writeAndVerifyData(logical_block_number_t startBlock,
                        block_count_t          blockOffset,
                        block_count_t          blockCount,
                        block_count_t          expectedBlocksFree,
                        block_count_t          expectedBlocksAllocated)
{
  writeData(startBlock, blockOffset, blockCount, VDO_SUCCESS);
  verifyWrite(startBlock, blockOffset, blockCount, expectedBlocksFree,
              expectedBlocksAllocated);
}

/**********************************************************************/
void trimAndVerifyData(logical_block_number_t startBlock,
                       block_count_t          blockCount,
                       block_count_t          expectedBlocksFree,
                       block_count_t          expectedBlocksAllocated)
{
  discardData(startBlock, blockCount, VDO_SUCCESS);
  verifyZeros(startBlock, blockCount);
  checkStats(expectedBlocksFree, expectedBlocksAllocated);
}
