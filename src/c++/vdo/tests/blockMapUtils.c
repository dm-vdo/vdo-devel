/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "blockMapUtils.h"

#include "memory-alloc.h"
#include "syscalls.h"

#include "block-map.h"
#include "completion.h"
#include "data-vio.h"

#include "asyncLayer.h"
#include "asyncVIO.h"
#include "ioRequest.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

typedef struct {
  struct block_map_entry mapping;
  int                    result;
} MappingExpectation;

static block_count_t                 logicalBlockCount;
static MappingExpectation           *expectedMappings;
static PopulateBlockMapConfigurator *populateConfigurator;
static struct zoned_pbn              lookupResult;

/**********************************************************************/
void initializeBlockMapUtils(block_count_t logicalBlocks)
{
  logicalBlockCount = logicalBlocks;
  VDO_ASSERT_SUCCESS(uds_allocate(logicalBlocks,
                                  MappingExpectation,
                                  __func__,
                                  &expectedMappings));
}

/**********************************************************************/
void tearDownBlockMapUtils(void)
{
  uds_free(uds_forget(expectedMappings));
}

/**********************************************************************/
static void compareMapping(struct vdo_completion *completion);

/**
 * Lookup up an LBN->PBN mapping in the block map.
 *
 * @param completion  The data_vio holding the LBN to look up
 **/
static void getMapping(struct vdo_completion *completion)
{
  completion->callback = compareMapping;
  completion->error_handler = compareMapping;
  vdo_get_mapped_block(as_data_vio(completion));
}

/**********************************************************************/
static bool replaceCallbackWithGetMapping(struct vdo_completion *completion)
{
  if (completion->callback == continue_data_vio_with_block_map_slot) {
    completion->callback = getMapping;
    removeCompletionEnqueueHook(replaceCallbackWithGetMapping);
  }

  return true;
}

/**********************************************************************/
static void lookupMapping(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = as_data_vio(completion);
  memset(&dataVIO->tree_lock, 0, sizeof(struct tree_lock));
  dataVIO->logical.zone
    = &vdo->logical_zones->zones[vdo_compute_logical_zone(dataVIO)];
  completion->requeue = true;
  addCompletionEnqueueHook(replaceCallbackWithGetMapping);
  vdo_find_block_map_slot(dataVIO);
}

/**
 * Check that looking up a mapping had the expected result. If that result was
 * a success, check that the LBN->PBN mapping is as expected.
 *
 * @param completion  The data_vio holding the LBN and mapping to check
 **/
static void compareMapping(struct vdo_completion *completion)
{
  struct data_vio    *dataVIO = as_data_vio(completion);
  MappingExpectation *expectation = &expectedMappings[dataVIO->logical.lbn];
  CU_ASSERT_EQUAL(expectation->result, completion->result);
  if (expectation->result == VDO_SUCCESS) {
    struct data_location expected
      = vdo_unpack_block_map_entry(&expectation->mapping);
    CU_ASSERT_EQUAL(expected.pbn,   dataVIO->mapped.pbn);
    CU_ASSERT_EQUAL(expected.state, dataVIO->mapped.state);
  } else {
    // Don't fail the entire operation.
    vdo_reset_completion(completion);
  }

  if (++dataVIO->logical.lbn >= logicalBlockCount) {
    complete_data_vio(completion);
    return;
  }

  lookupMapping(completion);
}

/**********************************************************************/
static void saveToBlockMap(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = as_data_vio(completion);
  populateConfigurator(dataVIO);
  completion->callback = complete_data_vio;
  vdo_put_mapped_block(dataVIO);
}

/**********************************************************************/
static void findBlockMapSlotAndSave(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = as_data_vio(completion);

  dataVIO->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
  completion->requeue = true;
  vdo_find_block_map_slot(as_data_vio(completion));
}

/**
 * Implements CompletionHook.
 **/
static bool populateBlockMapCallback(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_LAUNCH)) {
    struct data_vio *dataVIO = as_data_vio(completion);
    set_data_vio_logical_callback(dataVIO, findBlockMapSlotAndSave);
    // As noted below, we can't launch this as a write, but it needs to be a
    // write in order to update the block map. So we switch the operation here.
    dataVIO->read = false;
    dataVIO->write = true;
  } else if (completion->callback == continue_data_vio_with_block_map_slot) {
    completion->callback = saveToBlockMap;
  }

  completion->requeue = true;
  return true;
}

/**********************************************************************/
void populateBlockMap(logical_block_number_t start,
                      block_count_t count,
                      PopulateBlockMapConfigurator *configurator)
{
  populateConfigurator = configurator;
  addCompletionEnqueueHook(populateBlockMapCallback);

  /*
   * This can't be a write because the attempt to copy the data from the bio
   * blows up on the NULL buffer, and making a real buffer here is expensive
   * and wasteful.
   */
  VDO_ASSERT_SUCCESS(performRead(start, count, NULL));
  removeCompletionEnqueueHook(populateBlockMapCallback);
}

/**
 * Implements CompletionHook.
 **/
static bool verifyBlockMappingCallback(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_LAUNCH)) {
    set_data_vio_logical_callback(as_data_vio(completion), lookupMapping);
    removeCompletionEnqueueHook(verifyBlockMappingCallback);
  }

  return true;
}

/**********************************************************************/
static void lookupCallback(struct vdo_completion *completion)
{
  struct data_vio *dataVIO = as_data_vio(completion);

  // Set the requeue flag so that other hooks in the stack have a chance to
  // run.
  completion->requeue = true;

  switch (dataVIO->last_async_operation) {
  case VIO_ASYNC_OP_LAUNCH:
    dataVIO->last_async_operation = VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT;
    vdo_find_block_map_slot(dataVIO);
    return;

  case VIO_ASYNC_OP_FIND_BLOCK_MAP_SLOT:
    dataVIO->last_async_operation = VIO_ASYNC_OP_GET_MAPPED_BLOCK_FOR_READ;
    vdo_get_mapped_block(dataVIO);
    return;

  default:
    lookupResult = dataVIO->mapped;
    complete_data_vio(completion);
  }
}

/**
 * A hook to retrieve a mapping from the block map.
 *
 * Implements CompletionHook
 **/
static bool lookupLBNHook(struct vdo_completion *completion)
{
  if (lastAsyncOperationIs(completion, VIO_ASYNC_OP_LAUNCH)) {
    completion->callback = lookupCallback;
  } else if (completion->callback == continue_data_vio_with_block_map_slot) {
    completion->callback = lookupCallback;
    removeCompletionEnqueueHook(lookupLBNHook);
  }

  return true;
}

/**********************************************************************/
struct zoned_pbn lookupLBN(logical_block_number_t lbn)
{
  addCompletionEnqueueHook(lookupLBNHook);
  VDO_ASSERT_SUCCESS(performRead(lbn, 1, NULL));
  return lookupResult;
}

/**********************************************************************/
void verifyBlockMapping(logical_block_number_t start)
{
  addCompletionEnqueueHook(verifyBlockMappingCallback);
  VDO_ASSERT_SUCCESS(performRead(start, 1, NULL));
}

/**********************************************************************/
struct data_location getBlockMapping(logical_block_number_t lbn)
{
  return vdo_unpack_block_map_entry(&expectedMappings[lbn].mapping);
}

/**********************************************************************/
void setBlockMapping(logical_block_number_t   lbn,
                     physical_block_number_t  pbn,
                     enum block_mapping_state state)
{
  expectedMappings[lbn] = (MappingExpectation) {
    .mapping = vdo_pack_block_map_entry(pbn, state),
    .result  = VDO_SUCCESS,
  };
}

/**********************************************************************/
void setBlockMappingError(logical_block_number_t lbn, int error)
{
  expectedMappings[lbn].result = error;
}
