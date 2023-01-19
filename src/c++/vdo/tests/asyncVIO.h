/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef ASYNC_VIO_H
#define ASYNC_VIO_H

#include "permassert.h"

#include <linux/atomic.h>
#include <linux/bio.h>

#include "data-vio.h"
#include "types.h"

#include "callbackWrappingUtils.h"

/**
 * Wrap the callback of a vio.
 *
 * @param vio       The vio
 * @param callback  The wrapper callback
 **/
static inline void wrapVIOCallback(struct vio *vio, vdo_action *callback)
{
  wrapCompletionCallback(&vio->completion, callback);
}

/**
 * Check whether a completion is a vio.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is a vio
 **/
static inline bool is_vio(struct vdo_completion *completion)
{
  return (completion->type == VIO_COMPLETION);
}

/**
 * Check whether a completion is a data_vio.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is a data_vio
 **/
static inline bool isDataVIO(struct vdo_completion *completion)
{
  return (is_vio(completion) && is_data_vio(as_vio(completion)));
}

/**
 * Check whether a completion is a vio doing a metadata read.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is a vio doing a metadata read
 **/
static inline bool isMetadataRead(struct vdo_completion *completion)
{
  if (!is_vio(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  return (!is_data_vio(vio) && (bio_op(vio->bio) == REQ_OP_READ));
}

/**
 * Check whether a completion is a vio doing a metadata write.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is a vio doing a metadata write
 **/
static inline bool isMetadataWrite(struct vdo_completion *completion)
{
  if (!is_vio(completion)) {
    return false;
  }

  struct vio *vio = as_vio(completion);
  return (!is_data_vio(vio)
          && (bio_op(vio->bio) == REQ_OP_WRITE)
          && (vio->bio->bi_vcnt > 0));
}

/**
 * Get the PBN to which a vio is doing I/O.
 *
 * @param vio  The vio to check
 *
 * @return The pbn corresponding to the sector of the vio's bio
 **/
static inline physical_block_number_t pbnFromVIO(struct vio *vio)
{
  return pbn_from_vio_bio(vio->bio);
}

/**
 * Check whether the lastAsyncOperation of a data_vio matches a given type.
 *
 * @param completion      The completion to check
 * @param asyncOperation  The desired operation
 *
 * @return <code>true</code> if the completion is a data vio has the
 *         specified lastAsyncOperation
 **/
__attribute__((warn_unused_result))
static inline bool lastAsyncOperationIs(struct vdo_completion *completion,
                                        enum async_operation_number operation)
{
  return (isDataVIO(completion)
          && (as_data_vio(completion)->last_async_operation == operation));
}

/**
 * Check whether the logical address of a vio matches a given value.
 *
 * @param vio  The vio to check
 * @param lbn  The desired LBN
 *
 * @return <code>true</code> if the vio's logical field has the desired value
 **/
bool logicalIs(struct vdo_completion *completion, logical_block_number_t lbn)
  __attribute__((warn_unused_result));

/**
 * Check whether a completion is a vio of the given type.
 *
 * @param completion  The completion
 * @param expected    The expected type
 *
 * @return <code>true</code> if the completion is a vio with the expected
 *         vio_type
 **/
__attribute__((warn_unused_result))
static inline bool
vioTypeIs(struct vdo_completion *completion, enum vio_type expected)
{
  return (is_vio(completion) && (as_vio(completion)->type == expected));
}

/**
 * Check whether a completion is about to do a data write. Can be used as a
 * CompletionHook.
 *
 * @param completion  The completion to check
 *
 * @return <code>true</code> if the completion is a data_vio whose next
 *         action will be a data write
 **/
__attribute__((warn_unused_result))
static inline bool isDataWrite(struct vdo_completion *completion)
{
  return (lastAsyncOperationIs(completion, VIO_ASYNC_OP_WRITE_DATA_VIO)
          && (completion->callback_thread_id
              == get_vio_bio_zone_thread_id(as_vio(completion))));
}

/**
 * Set the result on a vio.
 *
 * @param vio     The vio to set
 * @param result  The result to set
 **/
static inline void setVIOResult(struct vio *vio, int result)
{
  vdo_set_completion_result(&vio->completion, result);
}

/**
 * Check whether a vio's bio has the preflush flag set.
 *
 * @param vio  The vio to check
 *
 * @return <code>true</code> if the vio's bio should flush
 **/
static inline bool isPreFlush(struct vio *vio)
{
  return ((vio->bio->bi_opf & REQ_PREFLUSH) == REQ_PREFLUSH);
}

#endif // ASYNC_VIO_H
