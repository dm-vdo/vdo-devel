/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * This is the test "Tracer" device, which is used to debug mismatch
 * problems in VDO.
 *
 * $Id$
 */

#include "dmTracer.h"

#include <linux/atomic.h>
#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/version.h>

#include "common.h"
#include "traceLoggerBlockTrace.h"

// Tracer instance sysfs node
static struct kobject tracerKobj;

/**********************************************************************/

#define DM_MSG_PREFIX  "tracer"
#define SYSFS_DIR_NAME "tracer"

#define MIN_IOS 64

enum {
  VDO_BLOCK_SIZE = 4096,
  VDO_SECTORS_PER_BLOCK = (VDO_BLOCK_SIZE >> SECTOR_SHIFT)
};


typedef struct tracerDevice {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev        *dev;
  // The sysfs node that connects /sys/<moduleName>/tracer/<tracerName> to this
  // device.
  struct kobject        kobj;
  // The name of the device.
  // The name is located immediately after the allocated structure.
  char                 *tracerName;
  // Pointer to the target's request queue
  struct request_queue *requestQueue;
  // Trace logging object used by this device.
  bool                  enabled;
  unsigned long         sectorCount;
  TraceLogger          *logger;
  // Bio set used for cloning bios
  struct bio_set        bs;

  // BEGIN data that are merely statistics and do not effect code behavior.
  // These stats count the bios that arrive into the tracerMap method.
  atomic64_t            readTotal;
  atomic64_t            writeTotal;
  atomic64_t            flushTotal;
  atomic64_t            fuaTotal;
  // END of statistics
} TracerDevice;

struct per_bio_data {
  struct bio *bioClone;
};

/**********************************************************************
 * Logs details of a bio through the current trace logger.
 *
 * @param [in] td  the tracer device instance
 * @param [in] bio the bio struct to log info about
 */
static inline int logBio(TracerDevice *td, struct bio *bio)
{
  int result = 0;

  if (td->enabled) {
    result = logBioDetails(td->logger, bio);
  }
  return result;
}

/**********************************************************************/
// BEGIN large section of code for the sysfs interface
/**********************************************************************/

typedef struct {
  struct attribute attr;
  ssize_t (*show)(TracerDevice *td, char *buf);
  ssize_t (*store)(TracerDevice *td, const char *value);
} TracerAttribute;

/**********************************************************************/
static void tracerRelease(struct kobject *kobj)
{
  TracerDevice *td = container_of(kobj, TracerDevice, kobj);
  kfree(td);
}

/**********************************************************************/
static ssize_t tracerShow(struct kobject   *kobj,
                          struct attribute *attr,
                          char             *buf)
{
  TracerDevice *td = container_of(kobj, TracerDevice, kobj);
  TracerAttribute *ta = container_of(attr, TracerAttribute, attr);
  if (ta->show != NULL) {
    return ta->show(td, buf);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t tracerShowStatistics(TracerDevice *td, char *buf)
{
  return sprintf(buf,
                 "reads: %lld\n"
                 "writes: %lld\n"
                 "flushes: %lld\n"
                 "FUAs: %lld\n",
                 (long long) atomic64_read(&td->readTotal),
                 (long long) atomic64_read(&td->writeTotal),
                 (long long) atomic64_read(&td->flushTotal),
                 (long long) atomic64_read(&td->fuaTotal));
}

/**********************************************************************/
static ssize_t tracerStore(struct kobject   *kobj,
                           struct attribute *attr,
                           const char       *buf,
                           size_t            length)
{
  TracerDevice *td = container_of(kobj, TracerDevice, kobj);
  TracerAttribute *ta = container_of(attr, TracerAttribute, attr);
  char *string = bufferToString(buf, length);
  ssize_t status;
  if (string == NULL) {
    status = -ENOMEM;
  } else if (ta->store != NULL) {
    status = ta->store(td, string);
  } else {
    status = -EINVAL;
  }
  kfree(string);
  return status ? status : length;
}


/**********************************************************************/

static TracerAttribute statisticsAttr = {
  .attr = { .name = "statistics", .mode = 0444 },
  .show = tracerShowStatistics,
};

static struct attribute *tracer_attrs[] = {
  &statisticsAttr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(tracer);

static struct sysfs_ops tracerOps = {
  .show  = tracerShow,
  .store = tracerStore,
};

static struct kobj_type tracerObjectType = {
  .release        = tracerRelease,
  .sysfs_ops      = &tracerOps,
  .default_groups = tracer_groups,
};

/**********************************************************************/
// BEGIN device methods for the tracer target type
/**********************************************************************/

/**********************************************************************/
static int tracerCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 3) {
    ti->error = "requires exactly 3 arguments";
    return -EINVAL;
  }
  const char *tracerName        = argv[0];
  int         tracerNameLength  = strlen(tracerName) + 1;
  const char *devicePath        = argv[1];

  // Check the sectors per block value
  unsigned long sectorCount = 1;
  if (sscanf(argv[2], "%lu", &sectorCount) != 1) {
    ti->error = "Sector count not a number";
    return -EINVAL;
  }

  if (sectorCount != 1 && sectorCount != VDO_SECTORS_PER_BLOCK) {
    ti->error = "Sector count should be either 1 or 8";
    return -EINVAL;
  }

  TracerDevice *td = kzalloc(sizeof(TracerDevice) + tracerNameLength,
                             GFP_KERNEL);
  if (td == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }

  td->tracerName = ((char *) td) + sizeof(TracerDevice);
  strncpy(td->tracerName, tracerName, tracerNameLength);

  // Tracing off by default
  td->enabled = false;

  if (dmGetDevice(ti, devicePath, &td->dev)) {
    ti->error = "Device lookup failed";
    kfree(td);
    return -EINVAL;
  }

  TraceLoggerBlockTraceCreationParameters loggerParams = {
    .tracerDevice = td,
  };
  int result = makeBlockTraceLogger(&loggerParams, &td->logger);
  if (result < 0) {
    ti->error = "logger creation failed";
    dm_put_device(ti, td->dev);
    kfree(td);
    return result;
  }

  kobject_init(&td->kobj, &tracerObjectType);
  result = kobject_add(&td->kobj, &tracerKobj, td->tracerName);
  if (result < 0) {
    int result2 = destroyTraceLogger(&td->logger);
    if (result2 < 0) {
      printk("failure to destroy logger, result = %d", result2);
    }
    ti->error = "sysfs addition failed";
    dm_put_device(ti, td->dev);
    kfree(td);
    return result;
  }

  result = bioset_init(&td->bs, MIN_IOS, 0, BIOSET_NEED_BVECS);
  if (result < 0) {
    ti->error = "Cannot allocate tracer bioset";
    dm_put_device(ti, td->dev);
    kobject_put(&td->kobj);
    kfree(td);
    return result;
  }

  // If this value changes, please make sure to update the
  // value for maxDiscardSectors accordingly.
  BUG_ON(dm_set_target_max_io_len(ti, VDO_SECTORS_PER_BLOCK) != 0);
  td->sectorCount = sectorCount;

  ti->discards_supported = 1;
  ti->num_discard_bios = 1;

  ti->flush_supported = 1;
  ti->num_flush_bios = 1;

  ti->per_io_data_size = sizeof(struct per_bio_data);

  ti->private = td;
  return 0;
}

/**********************************************************************/
static void tracerDtr(struct dm_target *ti)
{
  TracerDevice *td = ti->private;

  td->enabled = false;
  int result = destroyTraceLogger(&td->logger);
  if (result < 0) {
    printk("failure to destroy logger, result = %d", result);
  }
  dm_put_device(ti, td->dev);
  bioset_exit(&td->bs);
  kobject_put(&td->kobj);
}

/**********************************************************************/
static int tracerEndIo(struct dm_target *ti,
                       struct bio       *bio,
                       BioStatusType    *error)
 {
  TracerDevice *td = ti->private;

  struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
  bio = pb->bioClone;

  bool doWork = false;
  doWork = ((*error == BIO_SUCCESS) && isReadBio(bio));
  if (doWork) {
    int result = logBio(td, bio);
    if (result < 0) {
      printk("error logging read bio, result = %d", result);
    }
  }

  // Release the clone.
  bio_put(bio);

  return blk_status_to_errno(*error);
}

/**********************************************************************/
static void tracerIoHints(struct dm_target *ti, struct queue_limits *limits)
{
  TracerDevice *td = ti->private;
  unsigned long sectorCount = td->sectorCount;

  limits->logical_block_size  = sectorCount * SECTOR_SIZE;
  limits->physical_block_size = SECTOR_SIZE;

  // The minimum io size for random io
  blk_limits_io_min(limits, sectorCount * SECTOR_SIZE);
  // The optimal io size for streamed/sequential io
  blk_limits_io_opt(limits, VDO_BLOCK_SIZE);

  // Discard hints
  limits->max_discard_sectors = VDO_SECTORS_PER_BLOCK;
  limits->discard_granularity = VDO_BLOCK_SIZE;
}

/**********************************************************************/
static int tracerMap(struct dm_target *ti,
                     struct bio       *bio)
{
  TracerDevice *td = ti->private;

  // If we don't yet have the request queue (necessary for logging bio info)
  // associated with the device of this tracer instance get it via the bio.
  if (td->requestQueue == NULL) {
    td->requestQueue = bdev_get_queue(bio->bi_bdev);
  }

  // Map the I/O to the storage device.
  setBioBlockDevice(bio, td->dev->bdev);
  setBioSector(bio, dm_target_offset(ti, getBioSector(bio)));

  // Get a clone of the original bio for any necessary end io processing.
  struct bio *bioClone = cloneBio(bio, &td->bs);
  if (bioClone == NULL) {
    printk("failure to clone bio");
    return -ENOMEM;
  }
  struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
  pb->bioClone = bioClone;

  // Perform accounting.
  if (bio_data_dir(bio) == READ) {
    atomic64_inc(&td->readTotal);
  } else {
    if (isFlushBio(bio)) {
      atomic64_inc(&td->flushTotal);
    }
    if (isFUABio(bio)) {
      atomic64_inc(&td->fuaTotal);
    }
    if (getBioSize(bio) > 0) {
      atomic64_inc(&td->writeTotal);
    }
  }

  if (!isReadBio(bio)) {
    int result = logBio(td, bio);
    if (result < 0) {
      printk("error logging bio, result = %d", result);
    }
  }

  return DM_MAPIO_REMAPPED;
}

/**********************************************************************/
static int tracerMessage(struct dm_target  *ti,
                         unsigned int       argc,
                         char             **argv,
                         char              *resultBuffer,
                         unsigned int       maxlen)
{
  TracerDevice *td = ti->private;

  int result = 0;

  if (isGlobalDisableMessage(argc, argv)) {
    td->enabled = false;
  } else if (isGlobalEnableMessage(argc, argv)) {
    td->enabled = true;
  } else {
    result = -EINVAL;
    printk("unrecognized dmsetup message '%s' received\n", argv[0]);
  }
  return result;
}

/**********************************************************************/
static void tracerStatus(struct dm_target *ti,
                         status_type_t     type,
                         unsigned int      status_flags,
                         char             *result,
                         unsigned int      maxlen)
{
  TracerDevice *td = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro
  unsigned long sectorCount = td->sectorCount;

  switch (type) {
  case STATUSTYPE_INFO:
    DMEMIT("%s /dev/%pg %lu %s",
           td->tracerName,
           td->dev->bdev,
           sectorCount,
           td->enabled ? "on" : "off");
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s /dev/%pg %lu",
           td->tracerName,
           td->dev->bdev,
           sectorCount);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type tracerTargetType = {
  .name            = "tracer",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = tracerCtr,
  .dtr             = tracerDtr,
  .end_io          = tracerEndIo,
  .iterate_devices = commonIterateDevices,
  .io_hints        = tracerIoHints,
  .map             = tracerMap,
  .message         = tracerMessage,
  .status          = tracerStatus,
  .prepare_ioctl   = commonPrepareIoctl,
  // Put version specific functions at the bottom
};

/**********************************************************************/
int __init tracerInit(void)
{
  STATIC_ASSERT(offsetof(TracerDevice, dev)
                == offsetof(CommonDevice, dev));

  kobject_init(&tracerKobj, &emptyObjectType);
  int result = kobject_add(&tracerKobj, NULL, THIS_MODULE->name);
  if (result < 0) {
    return result;
  }

  result = dm_register_target(&tracerTargetType);
  if (result < 0) {
    kobject_put(&tracerKobj);
    DMERR("dm_register_target failed %d", result);
  }
  return result;
}

/**********************************************************************/
void __exit tracerExit(void)
{
  dm_unregister_target(&tracerTargetType);
  kobject_put(&tracerKobj);
}

/**********************************************************************/
struct request_queue *getTracerRequestQueue(struct tracerDevice *td)
{
  // Always use the request queue associated with the tracer device.
  // The bio may be modified during processing and tracing is performed
  // via the tracer device.

  return td->requestQueue;
}

/**********************************************************************/
const char *getTracerName(struct tracerDevice *td)
{
  return td->tracerName;
}

/**********************************************************************/
unsigned long getTracerSectorCount(struct tracerDevice *td)
{
  return td->sectorCount;
}

module_init(tracerInit);
module_exit(tracerExit);

MODULE_DESCRIPTION(DM_NAME " tracing test device");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
