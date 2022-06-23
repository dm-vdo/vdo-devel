/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "vdoVolumeUtils.h"

#include <err.h>

#include "errors.h"
#include "permassert.h"

#include "status-codes.h"

#include "fileLayer.h"
#include "userVDO.h"

static char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];

/**
 * Load a VDO from a file.
 *
 * @param [in]  filename        The file name
 * @param [in]  readOnly        Whether the layer should be read-only.
 * @param [in]  validateConfig  Whether the VDO should validate its config
 * @param [out] vdoPtr          A pointer to hold the VDO
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check loadVDOFromFile(const char *filename,
					bool readOnly,
					bool validateConfig,
					UserVDO **vdoPtr)
{
  int result = ASSERT(validateConfig || readOnly,
                      "Cannot make a writable VDO"
                      " without validating its config");
  if (result != UDS_SUCCESS) {
    return result;
  }

  PhysicalLayer *layer;
  if (readOnly) {
    result = makeReadOnlyFileLayer(filename, &layer);
  } else {
    result = makeFileLayer(filename, 0, &layer);
  }

  if (result != VDO_SUCCESS) {
    warnx("Failed to make FileLayer from '%s' with %s", filename,
          uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
    return result;
  }

  // Create the VDO.
  UserVDO *vdo;
  result = loadVDO(layer, validateConfig, &vdo);
  if (result != VDO_SUCCESS) {
    layer->destroy(&layer);
    warnx("loading VDO failed with: %s",
          uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
    return result;
  }

  *vdoPtr = vdo;
  return VDO_SUCCESS;
}

/**********************************************************************/
int makeVDOFromFile(const char *filename, bool readOnly, UserVDO **vdoPtr)
{
  return loadVDOFromFile(filename, readOnly, true, vdoPtr);
}

/**********************************************************************/
int readVDOWithoutValidation(const char *filename, UserVDO **vdoPtr)
{
  return loadVDOFromFile(filename, true, false, vdoPtr);
}

/**********************************************************************/
void freeVDOFromFile(UserVDO **vdoPtr)
{
  UserVDO *vdo = *vdoPtr;
  if (vdo == NULL) {
    return;
  }

  PhysicalLayer *layer = vdo->layer;
  freeUserVDO(&vdo);
  layer->destroy(&layer);
  *vdoPtr = NULL;
}
