/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 *
 * This is a test device which attempts to duplicate a scenario where we return
 * up stack on same thread as we submit a flush.
 *
 * This device only supports one thread submitting flush operations to it! Any
 * other setup will probably behave strangely.
 */

#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/version.h>

#include "common.h"
#include "limiter.h"

typedef struct {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev    *dev;
  // Sysfs handle
  struct kobject    kobj;
  // Cheap way to implement "block until another thread says go"
  Limiter           flushFreezer;
  // Flag indicating that freezing is enabled
  bool              freezeFlush;
  // Flag indicating a flush is currently frozen
  bool              flushFrozen;
} FlushDevice;

/**********************************************************************/

#define DM_MSG_PREFIX "flushnop"

static struct kobject flushKobj;

/**********************************************************************/
// BEGIN large section of code for the sysfs interface
/**********************************************************************/

typedef struct {
  struct attribute attr;
  ssize_t (*show)(FlushDevice *fd, char *buf);
  ssize_t (*store)(FlushDevice *fd, const char *value);
} FlushAttribute;

/**********************************************************************/
static void flushRelease(struct kobject *kobj)
{
  FlushDevice *device = container_of(kobj, FlushDevice, kobj);
  kfree(device);
}

/**********************************************************************/
static ssize_t flushShow(struct kobject   *kobj,
                         struct attribute *attr,
                         char             *buf)
{
  FlushDevice *device = container_of(kobj, FlushDevice, kobj);
  FlushAttribute *fa = container_of(attr, FlushAttribute, attr);
  if (fa->show != NULL) {
    return fa->show(device, buf);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t flushShowMode(FlushDevice *device, char *buf)
{
  strcpy(buf, device->flushFrozen ? "true\n" : "false\n");
  return strlen(buf);
}

/**********************************************************************/
static ssize_t flushStore(struct kobject   *kobj,
                          struct attribute *attr,
                          const char       *buf,
                          size_t            length)
{
  FlushDevice *device = container_of(kobj, FlushDevice, kobj);
  FlushAttribute *fa = container_of(attr, FlushAttribute, attr);
  char *string = bufferToString(buf, length);
  ssize_t status;
  if (string == NULL) {
    status = -ENOMEM;
  } else if (fa->store != NULL) {
    status = fa->store(device, string);
  } else {
    status = -EINVAL;
  }
  kfree(string);
  return status ? status : length;
}

/**********************************************************************/

static FlushAttribute flushModeAttr = {
  .attr = { .name = "frozen", .mode = 0444 },
  .show = flushShowMode,
};

static struct attribute *flush_attrs[] = {
  &flushModeAttr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(flush);

static struct sysfs_ops flushOps = {
  .show  = flushShow,
  .store = flushStore,
};

static struct kobj_type flushObjectType = {
  .release        = flushRelease,
  .sysfs_ops      = &flushOps,
  .default_groups = flush_groups,
};

/**********************************************************************/
static int flushCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 2) {
    ti->error = "requires exactly 2 arguments";
    return -EINVAL;
  }

  const char *flushName  = argv[0];
  const char *devicePath = argv[1];

  FlushDevice *device = kzalloc(sizeof(FlushDevice), GFP_KERNEL);
  if (device == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }

  if (dmGetDevice(ti, devicePath, &device->dev)) {
    ti->error = "Device lookup failed";
    kfree(device);
    return -EINVAL;
  }

  device->freezeFlush = false;
  initializeLimiter(&device->flushFreezer, 1);

  kobject_init(&device->kobj, &flushObjectType);
  int result = kobject_add(&device->kobj, &flushKobj, flushName);
  if (result < 0) {
    ti->error = "sysfs addition failed";
    dm_put_device(ti, device->dev);
    kfree(device);
    return result;
  }

  ti->flush_supported = 1;
  ti->num_discard_bios = 1;
  ti->num_flush_bios = 1;
  ti->private = device;
  return 0;
}

/**********************************************************************/
static void flushDtr(struct dm_target *ti)
{
  FlushDevice *device = ti->private;
  dm_put_device(ti, device->dev);
  kobject_put(&device->kobj);
}

/**
 * Handle an incoming I/O request. Everything but flushes are passed down to
 * the next device.
 *
 * Flush operations, if freezing is enabled (i.e., the freezeFlush flag is
 * set), will be blocked until freezing is disabled. Flush operations are not
 * passed down to the storage device; we always call bio_endio to indicate
 * success immediately; see test DeadlockAvoidance01 or ticket ESC-638.
 *
 * @param ti          The device's target info structure
 * @param bio         The incoming I/O request
 * @param mapContext  Ignored
 *
 * @return  device-mapper codes DM_MAPIO_SUBMITTED or _REMAPPED
 **/
static int flushMap(struct dm_target *ti, struct bio *bio)
{
  FlushDevice *device = ti->private;

  setBioBlockDevice(bio, device->dev->bdev);
  setBioSector(bio, dm_target_offset(ti, getBioSector(bio)));

  if (isFlushBio(bio)) {
    // Be sure our assumption that DM splits flush-with-data into a empty
    // flush followed by a pure data write is correct.
    BUG_ON(getBioSize(bio) > 0);
    if (device->freezeFlush) {
      DMERR_LIMIT("Freezing flush bio");
      device->flushFrozen = true;
      limiterWaitForOneFree(&device->flushFreezer);
      device->flushFrozen = false;
      DMERR_LIMIT("Done Freezing");
    }

    /*
     * Some variants all seem to work here: (1) call bio_endio reporting
     * success (and return SUBMITTED); (2) call bio_endio reporting -EIO
     * (remember to clear BIO_UPTODATE!); (3) return -EIO and let
     * submit_bio_noacct call bio_endio.
     */
    DMERR_LIMIT("calling bio_endio on a flush");
    endio(bio, BIO_SUCCESS);
    return DM_MAPIO_SUBMITTED;
  }

  return DM_MAPIO_REMAPPED;
}

/**********************************************************************/
static int flushMessage(struct dm_target  *ti,
                        unsigned int       argc,
                        char             **argv,
                        char              *resultBuffer,
                        unsigned int       maxlen)
{
  FlushDevice *device = ti->private;
  bool        invalidMessage = false;

  int result = 0;

  if (isGlobalDisableMessage(argc, argv)) {
    device->freezeFlush = false;
    limiterRelease(&device->flushFreezer);
    DMINFO("disable freezing");
  } else if (isGlobalEnableMessage(argc, argv)) {
    device->freezeFlush = true;
    limiterWaitForOneFree(&device->flushFreezer);
    DMINFO("enable freezing");
  } else {
    invalidMessage = true;
  }

  if (invalidMessage) {
    result = -EINVAL;
    DMERR("unrecognized dmsetup message '%s' received", argv[0]);
  }

  return result;
}

/**********************************************************************/
static void flushStatus(struct dm_target *ti,
                        status_type_t     type,
                        unsigned int      status_flags,
                        char             *result,
                        unsigned int      maxlen)
{
  FlushDevice *device = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro

  switch (type) {
  case STATUSTYPE_INFO:
    result[0] = '\0';
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s", device->dev->name); // XXX
    break;

  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type flushTargetType = {
  .name            = "flushnop",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = flushCtr,
  .dtr             = flushDtr,
  .iterate_devices = commonIterateDevices,
  .map             = flushMap,
  .message         = flushMessage,
  .status          = flushStatus,
  .prepare_ioctl   = commonPrepareIoctl,
  // Put version specific functions at the bottom
};

/**********************************************************************/
static int __init flushInit(void)
{
  BUILD_BUG_ON(offsetof(FlushDevice, dev) != offsetof(CommonDevice, dev));

  kobject_init(&flushKobj, &emptyObjectType);
  int result = kobject_add(&flushKobj, NULL, THIS_MODULE->name);
  if (result < 0) {
    return result;
  }

  result = dm_register_target(&flushTargetType);
  if (result < 0) {
    DMERR("dm_register_target failed %d", result);
  }
  return result;
}

/**********************************************************************/
static void __exit flushExit(void)
{
  dm_unregister_target(&flushTargetType);
  kobject_put(&flushKobj);
}

module_init(flushInit);
module_exit(flushExit);

MODULE_DESCRIPTION(DM_NAME " flushnop test device");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
