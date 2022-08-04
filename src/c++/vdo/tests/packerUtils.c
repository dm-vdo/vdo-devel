/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "packerUtils.h"

#include <linux/lz4.h>

#include "packer.h"
#include "vdo.h"

#include "asyncLayer.h"
#include "callbackWrappingUtils.h"
#include "lz4.h"
#include "mutexUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

static bool packingPrevented = false;
static bool reachedPacker;

/**
 * Action to flush the packer
 *
 * @param completion  The action completion
 **/
static void flushAction(struct vdo_completion *completion)
{
  vdo_flush_packer(vdo->packer);
  vdo_finish_completion(completion, VDO_SUCCESS);
}

/**********************************************************************/
void requestFlushPacker(void)
{
  performSuccessfulActionOnThread(flushAction, vdo->packer->thread_id);
}

/**********************************************************************/
bool isLeavingCompressor(struct vdo_completion *completion)
{
  return (lastAsyncOperationIs(completion, VIO_ASYNC_OP_COMPRESS_DATA_VIO)
          && (completion->callback_thread_id
              == vdo->thread_config->packer_thread)
          && (vdo_get_callback_thread_id() == vdo->thread_config->cpu_thread));
}

/**
 * Check whether a vio is just about to leave the compressor.
 *
 * Implements BlockCondition.
 **/
static bool

isLeavingCompressorBlockCondition(struct vdo_completion *completion,
                                  void *context __attribute__((unused)))
{
  return isLeavingCompressor(completion);
}

/**********************************************************************/
void setupCompressorLatch(void)
{
  setBlockVIOCompletionEnqueueHook(isLeavingCompressorBlockCondition, false);
}

/**********************************************************************/
void tearDownCompressorLatch(void)
{
  clearCompletionEnqueueHooks();
}

/**********************************************************************/
void waitForVIOLatchesAtCompressor(void)
{
  waitForBlockedVIO();
}

/**********************************************************************/
void releaseVIOLatchedInCompressor(void)
{
  releaseBlockedVIO();
}

/**
 * Record in the VIO that it has reached the packer and is waiting there.
 *
 * Implements VDOAction.
 **/
static void setReachedPacker(struct vdo_completion *completion)
{
  runSavedCallbackAssertNoRequeue(completion);
  signalState(&reachedPacker);
}

/**
 * Implements CompletionHook.
 **/
static bool wrapIfLeavingCompressor(struct vdo_completion *completion)
{
  if (isLeavingCompressor(completion)) {
    wrapCompletionCallback(completion, setReachedPacker);
  }

  return true;
}

/**********************************************************************/
void setupPackerNotification(void)
{
  reachedPacker = false;
  setCompletionEnqueueHook(wrapIfLeavingCompressor);
}

/**********************************************************************/
void tearDownPackerNotification(void)
{
  clearCompletionEnqueueHooks();
}

/**********************************************************************/
void waitForDataVIOToReachPacker(void)
{
  waitForStateAndClear(&reachedPacker);
}

/**
 * Wrap the user space lz4 compressor to have the same interface as the one
 * we use in the kernel.
 **/
int LZ4_compress_default(const char *source,
                         char *dest,
                         int isize,
                         int maxOutputSize,
                         void *context)
{
  return (READ_ONCE(packingPrevented)
          ? VDO_BLOCK_SIZE
          : LZ4_compress_ctx_limitedOutput(context,
                                           source,
                                           dest,
                                           isize,
                                           maxOutputSize));
}

/**********************************************************************/
int LZ4_decompress_safe(const char *source,
                        char *dest,
                        int isize,
                        int maxOutputSize)
{
  return LZ4_uncompress_unknownOutputSize(source,
                                          dest,
                                          isize,
                                          maxOutputSize);
}

/**********************************************************************/
void preventPacking(void)
{
  WRITE_ONCE(packingPrevented, true);
}

/**********************************************************************/
void restorePacking(void)
{
  WRITE_ONCE(packingPrevented, false);
}
