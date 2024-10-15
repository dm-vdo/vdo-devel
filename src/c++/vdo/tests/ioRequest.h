/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef IO_REQUEST_H
#define IO_REQUEST_H

#include <linux/blk_types.h>

#include "asyncLayer.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef uint32_t         BIOCount;
typedef struct ioRequest IORequest;

/**
 * A BIO wraps a struct bio in order to associate it with an IORequest (which
 * may group several testBIOs together), and to keep track of whether the bio
 * has been acknowledged.
 *
 * For now, the testBIO also holds the dataVIO which will service the bio,
 * but this will go away once the data_vio_pool is introduced.
 **/
typedef struct testBIO {
  /* The bio this wraps */
  struct bio      *bio;
  /* The request to which this bio belongs */
  IORequest       *request;
  /* Whether the bio has been acknowledged */
  bool             acknowledged;
  /* The next DataBIO in the request */
  struct testBIO  *next;
} BIO;

/**
 * An IORequest is used by unit tests to issue reads, writes, and discards
 * to a VDO. It groups together a set of testBIOs to cover the range of the
 * request.
 *
 * An IORequest is used by tests to wait on an I/O being complete, which
 * entails not only the bio being acknowledged, but all of the data vios used
 * to service them having finished their work.
 **/
struct ioRequest{
  /*
   * Success or the first error from an acknowledgment or data vio
   * completion
   */
  int            result;
  /* The number of bios in this request */
  BIOCount       bioCount;
  /* The number of acknowledged bios */
  BIOCount       acknowledgedCount;
  /* The number of completed data_vios */
  block_count_t  completeCount;
  /* The DataBios in this request */
  BIO           *bios;
};

/**
 * Free an IORequest.
 *
 * @param request  The request to free.
 **/
void freeRequest(IORequest *request);

/**
 * A callback to run when a data vio is complete, registered in
 * runEnqueueHook() for newly launched data vios.
 *
 * <p>Implements vdo_action_fn.
 **/
void dataVIOCompleteCallback(struct vdo_completion *completion);

/**
 * Create an IORequest backed by a supplied buffer and launch it. The operation
 * need not be block aligned.
 *
 * @param start      The sector at which the request's operation starts
 * @param count      The number of sectors in the request
 * @param buffer     The buffer backing the request
 * @param operation  The operation the request is to perform
 *
 * @return The request
 **/
IORequest *launchUnalignedBufferBackedRequest(sector_t  start,
                                              sector_t  count,
                                              char     *buffer,
                                              uint32_t  operation)
  __attribute__((warn_unused_result));

/**
 * Create an IORequest backed by a buffer and launch it.
 *
 * @param  start      The logical block at which the request's operation starts
 * @param  count      The number of blocks in the request
 * @param  buffer     The buffer backing the request
 * @param  operation  The operation the request is to perform
 *
 * @return The request
 **/
IORequest *launchBufferBackedRequest(logical_block_number_t  start,
                                     block_count_t           count,
                                     char                   *buffer,
                                     uint32_t                operation)
  __attribute__((warn_unused_result));

/**
 * Create an IORequest to write auto-generated test data and launch it.
 *
 * @param start  The logical block at which the request's operation starts
 * @param count  The number of blocks in the request
 * @param index  The index for generating the data to write
 *
 * @return The request
 **/
IORequest *launchIndexedWrite(logical_block_number_t start,
                              block_count_t          count,
                              block_count_t          index)
  __attribute__((warn_unused_result));

/**
 * Launch an asynchronous trim but don't wait for the result. The default
 * max discard size will be used.
 *
 * <p>The request is enqueued and a pointer to it is returned. The caller
 * must free the request once it has completed.
 *
 * @param start  The starting sector
 * @param count  The number of sectots to trim
 *
 * @return The request
 **/
IORequest *launchUnalignedTrim(sector_t start, sector_t count)
  __attribute__((warn_unused_result));

/**
 * Launch an asynchronous trim but don't wait for the result. The default
 * max discard size will be used.
 *
 * <p>The request is enqueued and a pointer to it is returned. The caller
 * must free the request once it has completed.
 *
 * @param start  The starting block number
 * @param count  The number of blocks to trim
 *
 * @return The request
 **/
IORequest *launchTrim(logical_block_number_t start, block_count_t count)
  __attribute__((warn_unused_result));

/**
 * Await the completion of an IORequest.
 *
 * @param request  The request to wait to complete
 *
 * @return the result from the completion
 **/
int awaitRequest(IORequest *request);

/**
 * Await the completion of an IORequest and assert that it succeeded.
 *
 * @param request  The request to wait to complete
 **/
static inline void awaitSuccessfulRequest(IORequest *request)
{
  VDO_ASSERT_SUCCESS(awaitRequest(request));
}

/**
 * Await the completion of an IORequest and then free it.
 *
 * @param request  The request to wait to complete
 *
 * @return the result from the completion
 **/
static inline int awaitAndFreeRequest(IORequest *request)
{
  int result = awaitRequest(request);
  freeRequest(request);
  return result;
}

/**
 * Await the completion of an IORequest, assert that it succeeded, and free it.
 *
 * @param request  The request to wait to complete
 **/
static inline void awaitAndFreeSuccessfulRequest(IORequest *request)
{
  VDO_ASSERT_SUCCESS(awaitAndFreeRequest(request));
}

/**
 * Perform an asynchronous VDO trim operation and await the result. The default
 * max discard size will be used.
 *
 * <p>The request is enqueued and this function waits for the trim to execute,
 * using a private callback. The request result is returned, and the request is
 * destroyed.
 *
 * @param start  The starting block number
 * @param count  The number of blocks to trim
 *
 * @return The success or failure of the action
 **/
static inline int performTrim(logical_block_number_t start,
                              block_count_t count)
{
  return awaitAndFreeRequest(launchTrim(start, count));
}

/**
 * Read data into a buffer.
 *
 * @param start   The LBN at which to start reading
 * @param count   The number of blocks to read
 * @param buffer  The buffer in which to store the data
 *
 * @return VDO_SUCCESS or an error
 **/
int performRead(logical_block_number_t  start,
                block_count_t           count,
                char                   *buffer)
  __attribute__((warn_unused_result));

/**
 * Write data from a buffer.
 *
 * @param start   The LBN at which to start writing
 * @param count   The number of blocks to write
 * @param buffer  The buffer of data to write
 *
 * @return VDO_SUCCESS or an error
 **/
int performWrite(logical_block_number_t  start,
                 block_count_t           count,
                 char                   *buffer)
  __attribute__((warn_unused_result));

/**
 * Write data from the pre-formatted test data blocks.
 *
 * @param start  The LBN at which to start writing
 * @param count  The number of blocks to write
 * @param index  The index of the first data block
 *
 * @return VDO_SUCCESS or an error
 **/
int performIndexedWrite(logical_block_number_t start,
                        block_count_t          count,
                        block_count_t          index)
  __attribute__((warn_unused_result));

/**
 * Write auto-generated data.
 *
 * @param start           The LBN at which to start writing
 * @param index           The index of the first data block
 * @param count           The number of blocks to write
 * @param expectedResult  The expected result of the write
 **/
void writeData(logical_block_number_t start,
               block_count_t          index,
               block_count_t          count,
               int                    expectedResult);

/**
 * Overwrite a range of logical blocks with zeros.
 *
 * @param startBlock      The logical block at which to start writing
 * @param blockCount      The number of blocks to write
 * @param expectedResult  The expected result of the write
 **/
void zeroData(logical_block_number_t startBlock,
              block_count_t          blockCount,
              int                    expectedResult);

/**
 * Discard a range of blocks with the default discard size.
 *
 * @param startBlock      The logical block at which to start writing
 * @param blockCount      The number of blocks to write
 * @param expectedResult  The expected result of the discard
 **/
static inline void discardData(logical_block_number_t startBlock,
                               block_count_t          blockCount,
                               int                    expectedResult)
{
  CU_ASSERT_EQUAL(performTrim(startBlock, blockCount), expectedResult);
}

/**
 * Verify the data we wrote.
 *
 * @param startBlock   The logical block at which to start reading
 * @param blockOffset  The offset into data of the first block to expect
 * @param count        The number of blocks to verify
 **/
void verifyData(logical_block_number_t startBlock,
                block_count_t          blockOffset,
                block_count_t          count);

/**
 * Verify that a given range of data is zero filled.
 *
 * @param startBlock  The logical block at which to start reading
 * @param count       The number of blocks to verify
 **/
void verifyZeros(logical_block_number_t startBlock, block_count_t count);

/**
 * Verify that a request was written correctly and that allocation is as
 * expected.
 *
 * @param startBlock               The logical block at which the write starts
 * @param blockOffset              The offset into data of the write's content
 * @param blockCount               The number of blocks to write
 * @param expectedBlocksFree       The expected number of free blocks
 * @param expectedBlocksAllocated  The expected number of blocks
 *                                 allocated
 **/
void verifyWrite(logical_block_number_t startBlock,
                 block_count_t          blockOffset,
                 block_count_t          blockCount,
                 block_count_t          expectedBlocksFree,
                 block_count_t          expectedBlocksAllocated);

/**
 * Write data to the VDO and verify it can be read back.
 *
 * @param startBlock               The logical block at which to start writing
 * @param blockOffset              The offset into data of the first block to
 *                                 write
 * @param blockCount               The number of blocks to write
 * @param expectedBlocksFree       The expected number of free blocks after
 *                                 writing
 * @param expectedBlocksAllocated  The expected number of blocks allocated
 *                                 after writing
 *
 **/
void writeAndVerifyData(logical_block_number_t startBlock,
                        block_count_t          blockOffset,
                        block_count_t          blockCount,
                        block_count_t          expectedBlocksFree,
                        block_count_t          expectedBlocksAllocated);

/**
 * Trim a range of lbns and verify zeros are read back.
 *
 * @param startBlock               The logical block at which to start trimming
 * @param blockCount               The number of blocks to trim
 * @param expectedBlocksFree       The expected number of free blocks after
 *                                 trimming
 * @param expectedBlocksAllocated  The expected number of blocks allocated
 *                                 after trimming
 **/
void trimAndVerifyData(logical_block_number_t startBlock,
                       block_count_t          blockCount,
                       block_count_t          expectedBlocksFree,
                       block_count_t          expectedBlocksAllocated);

#endif /* IO_REQUEST_H */
