/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "userVDO.h"

#include <err.h>

#include <linux/log2.h>

#include "memory-alloc.h"

#include "encodings.h"
#include "status-codes.h"
#include "types.h"

#include "physicalLayer.h"

/**********************************************************************/
int makeUserVDO(PhysicalLayer *layer, UserVDO **vdoPtr)
{
  UserVDO *vdo;
  int result = UDS_ALLOCATE(1, UserVDO, __func__, &vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  vdo->layer = layer;
  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
void freeUserVDO(UserVDO **vdoPtr)
{
  UserVDO *vdo = *vdoPtr;
  if (vdo == NULL) {
    return;
  }

  vdo_destroy_component_states(&vdo->states);
  UDS_FREE(vdo);
  *vdoPtr = NULL;
}

/**********************************************************************/
int __must_check loadSuperBlock(UserVDO *vdo)
{
  int result = vdo->layer->reader(vdo->layer,
                                  vdo_get_data_region_start(vdo->geometry), 1,
                                  vdo->superBlockBuffer);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return vdo_decode_super_block((u8 *) vdo->superBlockBuffer);
}

/**********************************************************************/
int loadVDOWithGeometry(PhysicalLayer           *layer,
                        struct volume_geometry  *geometry,
                        bool                     validateConfig,
                        UserVDO                **vdoPtr)
{
  UserVDO *vdo;
  int result = makeUserVDO(layer, &vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  vdo->geometry = *geometry;
  result = loadSuperBlock(vdo);
  if (result != VDO_SUCCESS) {
    freeUserVDO(&vdo);
    return result;
  }

  result = vdo_decode_component_states((u8 *) vdo->superBlockBuffer, &vdo->geometry, &vdo->states);
  if (result != VDO_SUCCESS) {
    freeUserVDO(&vdo);
    return result;
  }

  if (validateConfig) {
    result = vdo_validate_component_states(&vdo->states,
                                           geometry->nonce,
                                           layer->getBlockCount(layer),
                                           0);
    if (result != VDO_SUCCESS) {
      freeUserVDO(&vdo);
      return result;
    }
  }

  setDerivedSlabParameters(vdo);

  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
int loadVDO(PhysicalLayer *layer, bool validateConfig, UserVDO **vdoPtr)
{
  struct volume_geometry geometry;
  int result = vdo_load_volume_geometry(layer, &geometry);
  if (result != VDO_SUCCESS) {
    return result;
  }

  return loadVDOWithGeometry(layer, &geometry, validateConfig, vdoPtr);
}

/**********************************************************************/
int saveSuperBlock(UserVDO *vdo)
{
  vdo_encode_super_block((u8 *) vdo->superBlockBuffer, &vdo->states);
  return vdo->layer->writer(vdo->layer,
                            vdo_get_data_region_start(vdo->geometry),
                            1,
                            vdo->superBlockBuffer);
}

/**********************************************************************/
int saveVDO(UserVDO *vdo, bool saveGeometry)
{
  int result = saveSuperBlock(vdo);
  if (result != VDO_SUCCESS) {
    return result;
  }

  if (!saveGeometry) {
    return VDO_SUCCESS;
  }

  return vdo_write_volume_geometry(vdo->layer, &vdo->geometry);
}

/**********************************************************************/
void setDerivedSlabParameters(UserVDO *vdo)
{
  vdo->slabSizeShift = ilog2(vdo->states.vdo.config.slab_size);
  vdo->slabCount = vdo_compute_slab_count(vdo->states.slab_depot.first_block,
                                          vdo->states.slab_depot.last_block,
                                          vdo->slabSizeShift);
  vdo->slabOffsetMask = (1ULL << vdo->slabSizeShift) - 1;
}

/**********************************************************************/
int getSlabNumber(const UserVDO           *vdo,
                  physical_block_number_t  pbn,
                  slab_count_t            *slabPtr)
{
  const struct slab_depot_state_2_0 *depot = &vdo->states.slab_depot;
  if ((pbn < depot->first_block) || (pbn >= depot->last_block)) {
    return VDO_OUT_OF_RANGE;
  }

  *slabPtr = ((pbn - depot->first_block) >> vdo->slabSizeShift);
  return VDO_SUCCESS;
}

/**********************************************************************/
int getSlabBlockNumber(const UserVDO           *vdo,
                       physical_block_number_t  pbn,
                       slab_block_number       *sbnPtr)
{
  const struct slab_depot_state_2_0 *depot = &vdo->states.slab_depot;
  if ((pbn < depot->first_block) || (pbn >= depot->last_block)) {
    return VDO_OUT_OF_RANGE;
  }

  slab_block_number sbn = ((pbn - depot->first_block) & vdo->slabOffsetMask);
  if (sbn >= depot->slab_config.data_blocks) {
    return VDO_OUT_OF_RANGE;
  }

  *sbnPtr = sbn;
  return VDO_SUCCESS;
}

/**********************************************************************/
bool isValidDataBlock(const UserVDO *vdo, physical_block_number_t pbn)
{
  slab_block_number sbn;
  return (getSlabBlockNumber(vdo, pbn, &sbn) == VDO_SUCCESS);
}

/**********************************************************************/
const struct partition *
getPartition(const UserVDO     *vdo,
             enum partition_id  id,
             const char        *errorMessage)
{
  struct partition *partition;
  struct layout layout = vdo->states.layout;
  int result = vdo_get_partition(&layout, id, &partition);
  if (result != VDO_SUCCESS) {
    errx(1, "%s", errorMessage);
  }

  return partition;
}
