/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * This is a test "fua" device, which manages FUA (and sometimes flush)
 * bits on write requests. It can be used in these fashions:
 *
 * 1 - Atop VDO, with frequency 1, testing how VDO behaves with every
 *     write having the FUA bit set.
 *
 * 2 - Beneath VDO, especially on async storage, in tests where VDO's
 *     data persistence across a crash doesn't matter. With setting
 *     frequency set to 0, all FUA bits are stripped off incoming writes
 *     and every flush is instantly finished. This ruins VDO's data
 *     persistence guarantees on async storage, but significantly
 *     speeds up tests on storage where flushes and FUAs have a major
 *     cost.
 *
 * $Id$
 */

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/version.h>

#include "common.h"
#include "dmFua.h"

/**********************************************************************/

#define DM_MSG_PREFIX "fua"

typedef struct {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev *dev;
  atomic_t       counter;
  int            frequency;
} FuaDevice;

/**********************************************************************/
static int fuaCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 2) {
    ti->error = "requires exactly 2 arguments";
    return -EINVAL;
  }
  const char *devicePath = argv[0];
  unsigned long long frequency;
  char dummy;
  if ((sscanf(argv[1], "%llu%c", &frequency, &dummy) != 1)
      || ((unsigned long long) (unsigned int) frequency != frequency)) {
    ti->error = "Invalid FUA frequency";
    return -EINVAL;
  }

  FuaDevice *fd = kzalloc(sizeof(FuaDevice), GFP_KERNEL);
  if (fd == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }
  fd->frequency = frequency;

  if (dmGetDevice(ti, devicePath, &fd->dev)) {
    ti->error = "Device lookup failed";
    kfree(fd);
    return -EINVAL;
  }

  ti->num_discard_bios = 1;
  ti->num_flush_bios = 1;
  ti->private = fd;
  return 0;
}

/**********************************************************************/
static void fuaDtr(struct dm_target *ti)
{
  FuaDevice *fd = ti->private;
  dm_put_device(ti, fd->dev);
  kfree(fd);
}

/**********************************************************************/
static int fuaMap(struct dm_target *ti, struct bio *bio)
{
  FuaDevice *fd = ti->private;
  setBioBlockDevice(bio, fd->dev->bdev);
  setBioSector(bio, dm_target_offset(ti, getBioSector(bio)));
  if ((fd->frequency == 0) && isFlushBio(bio)) {
    endio(bio, BIO_SUCCESS);
    return DM_MAPIO_SUBMITTED;
  }

  // kernel 6.3.11 only allows _WRITE or _ZONE_APPEND to carry FUA/PREFLUSH,
  // not DISCARD
  if (bio_op(bio) == REQ_OP_WRITE) {
    if ((fd->frequency != 0)
        && (atomic_inc_return(&fd->counter) % fd->frequency == 0)) {
      bio->bi_opf |= REQ_FUA;
    } else {
      bio->bi_opf &= ~REQ_FUA;
    }
  }
  return DM_MAPIO_REMAPPED;
}

/**********************************************************************/
static void fuaStatus(struct dm_target *ti,
                      status_type_t     type,
                      unsigned int      status_flags,
                      char             *result,
                      unsigned int      maxlen)
{
  FuaDevice *fd = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro

  switch (type) {
  case STATUSTYPE_INFO:
    result[0] = '\0';
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s %llu", fd->dev->name, (unsigned long long)fd->frequency);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type fuaTargetType = {
  .name            = "fua",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = fuaCtr,
  .dtr             = fuaDtr,
  .iterate_devices = commonIterateDevices,
  .map             = fuaMap,
  .status          = fuaStatus,
  .prepare_ioctl   = commonPrepareIoctl,
  // Put version specific functions at the bottom
};

/**********************************************************************/
int __init fuaInit(void)
{
  STATIC_ASSERT(offsetof(FuaDevice, dev) 
                == offsetof(CommonDevice, dev)); 

  int result = dm_register_target(&fuaTargetType);
  if (result < 0) {
    DMERR("dm_register_target failed %d", result);
  }
  return result;
}

/**********************************************************************/
void __exit fuaExit(void)
{
  dm_unregister_target(&fuaTargetType);
}

module_init(fuaInit);
module_exit(fuaExit);

MODULE_DESCRIPTION(DM_NAME " fua testing device");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
