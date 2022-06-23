/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 *
 * Common code for all modules, which contain device mapper devices
 * used by VDO test code.  This file provides:
 *
 *  - Code common to different devices.
 *  - Module code and data, including an empty module sysfs inode.
 */

#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/version.h>

#include "common.h"

/**********************************************************************/
char *bufferToString(const char *buf, size_t length)
{
  char *string = kzalloc(length + 1, GFP_KERNEL);
  if (string != NULL) {
    memcpy(string, buf, length);
    if (string[length - 1] == '\n') {
      string[length - 1] = '\0';
    }
  }
  return string;
}

/**********************************************************************/
int commonPrepareIoctl(struct dm_target *ti, struct block_device **bdev)
{
  CommonDevice *cd = (CommonDevice *)ti->private;
  struct dm_dev *dev = cd->dev;

  *bdev = dev->bdev;

  // Only pass ioctls through if the device sizes match exactly.
  if (ti->len != i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT) {
    return 1;
  }
  return 0;
}

/**********************************************************************/
int commonIterateDevices(struct dm_target           *ti,
                         iterate_devices_callout_fn  fn,
                         void                       *data)
{
  CommonDevice *cd = ti->private;
  struct dm_dev *dev = cd->dev;

  return fn(ti, dev, 0, ti->len, data);
}

/**********************************************************************/
int dmGetDevice(struct dm_target  *ti,
                const char        *path,
                struct dm_dev    **devPtr)
{
  return dm_get_device(ti, path, dm_table_get_mode(ti->table), devPtr);
}

/**********************************************************************/
static void emptyRelease(struct kobject *kobj)
{
}

/**********************************************************************/
static ssize_t emptyShow(struct kobject   *kobj,
                         struct attribute *attr,
                         char             *buf)
{
  return 0;
}

/**********************************************************************/
static ssize_t emptyStore(struct kobject   *kobj,
                          struct attribute *attr,
                          const char       *buf,
                          size_t            length)
{
  return length;
}

/**********************************************************************/

static struct attribute *empty_attrs[] = {
  NULL,
};
ATTRIBUTE_GROUPS(empty);

static struct sysfs_ops emptyOps = {
  .show  = emptyShow,
  .store = emptyStore,
};

struct kobj_type emptyObjectType = {
  .release        = emptyRelease,
  .sysfs_ops      = &emptyOps,
  .default_groups = empty_groups,
};

struct kobject topKobj;
