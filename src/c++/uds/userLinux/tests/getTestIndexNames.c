/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testPrototypes.h"

#include <err.h>
#include <stdlib.h>

#include "assertions.h"
#include "fileUtils.h"
#include "logger.h"
#include "memory-alloc.h"

static char *testIndexNames[3] = {NULL, NULL, NULL};

/**********************************************************************/
char * const *getTestIndexNames(void)
{
  if (testIndexNames[0] == NULL) {
    testIndexNames[0] = getenv("UDS_TESTINDEX");
    if (testIndexNames[0] == NULL) {
      testIndexNames[0] = "/u1/zubenelgenubi";
    }

    if (asprintf(&testIndexNames[1], "%s%s", testIndexNames[0], "-1") == -1) {
      err(1, "Failed to allocate test device name");
    }

    testIndexNames[2] = NULL;
  }

  return testIndexNames;
}

/**********************************************************************/
void freeTestIndexNames(void)
{
  /* Only the second name required memory allocation. */
  free(testIndexNames[1]);
}

/**********************************************************************/
static struct block_device *getDeviceFromName(const char *name)
{
  int result;
  int fd;
  struct block_device *device;

  result = open_file(name, FU_READ_WRITE, &fd);
  if (result != UDS_SUCCESS) {
    vdo_log_error_strerror(result, "%s is not a block device", name);
    return NULL;
  }

  result = vdo_allocate(1, __func__, &device);
  if (result != VDO_SUCCESS) {
    vdo_log_error_strerror(ENOMEM, "cannot allocate device for %s", name);
    close_file(fd, NULL);
    return NULL;
  }

  device->fd = fd;
  device->size = (loff_t) SIZE_MAX;
  return device;
}

/**********************************************************************/
struct block_device *getTestBlockDevice(void)
{
  char *const *names = getTestIndexNames();

  return getDeviceFromName(names[0]);
}

/**********************************************************************/
struct block_device **getTestMultiBlockDevices(void)
{
  char *const *names = getTestIndexNames();
  static struct block_device *bdevs[2];

  bdevs[0] = getDeviceFromName(names[0]);
  bdevs[1] = getDeviceFromName(names[1]);
  return bdevs;
}

/**********************************************************************/
void putTestBlockDevice(struct block_device *bdev)
{
  if (bdev == NULL)
    return;

  close_file(bdev->fd, NULL);
  vdo_free(bdev);
}
