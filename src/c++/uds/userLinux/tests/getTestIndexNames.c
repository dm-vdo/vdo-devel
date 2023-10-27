/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testPrototypes.h"

#include <stdlib.h>

#include "assertions.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"

static const char *testIndexName = NULL;

/**********************************************************************/
const char * const *getTestIndexNames(void)
{
  if (testIndexName == NULL) {
    testIndexName = getenv("UDS_TESTINDEX");
    if (testIndexName == NULL) {
      testIndexName = "/u1/zubenelgenubi";
    }
  }

  static const char *names[2];
  names[0] = testIndexName;
  names[1] = NULL;

  return names;
}

/**********************************************************************/
const char *const *getTestMultiIndexNames(void)
{
  static const char *const names[3] = {
    "/u1/zubenelgenubi-0",
    "/u1/zubenelgenubi-1",
    NULL,
  };
  return names;
}

/**********************************************************************/
static struct block_device *getDeviceFromName(const char *name)
{
  int result;
  int fd;
  struct block_device *device;

  result = open_file(name, FU_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    uds_log_error_strerror(result, "%s is not a block device", name);
    return NULL;
  }

  result = UDS_ALLOCATE(1, struct block_device, __func__, &device);
  if (result != UDS_SUCCESS) {
    uds_log_error_strerror(ENOMEM, "cannot allocate device for %s", name);
    close_file(fd, NULL);
    return NULL;
  }

  device->fd = fd;
  return device;
}

/**********************************************************************/
struct block_device *getTestBlockDevice(void)
{
  if (testIndexName == NULL) {
    testIndexName = getenv("UDS_TESTINDEX");
    if (testIndexName == NULL) {
      testIndexName = "/u1/zubenelgenubi";
    }
  }

  return getDeviceFromName(testIndexName);
}

/**********************************************************************/
struct block_device **getTestMultiBlockDevices(void)
{
  static struct block_device *bdevs[2];

  bdevs[0] = getDeviceFromName("/u1/zubenelgenubi-0");
  bdevs[1] = getDeviceFromName("/u1/zubenelgenubi-1");
  return bdevs;
}

/**********************************************************************/
void putTestBlockDevice(struct block_device *bdev)
{
  if (bdev == NULL)
    return;

  close_file(bdev->fd, NULL);
  uds_free(bdev);
}
