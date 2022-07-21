/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * This is the test "Corruptor" device, which is used to corrupt data on read
 * and/or write.
 *
 * $Id$
 */

#include "dmCorruptor.h"

#include <linux/atomic.h>
#include <linux/blktrace_api.h>
#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "bioIterator.h"
#include "common.h"

/**
 * The corruptor device mapper target supports corrupting data on a per
 * sector basis for both read and write.  One can corrupt reads or writes
 * or both.
 *
 * By default the creation of a corruptor target does not immediately commence
 * corruption.  One must explicitly enable the desired corruption as well as
 * providing a 'frequency' of corruption.  This can be done either via
 * dmsetup messages or the created sysfs entries.
 *
 * One configures corruption by specifying the corruption type (default is
 * random) and frequency and explicitly enabling corruption.  The latter step
 * allows for corruption to be enabled and disabled w/o modifying the
 * corruption parameters.
 *
 * Corruption Type: Modulo
 *  If sector number modulo frequency is zero the sector is corrupted.
 *
 * Corruption Type: Random
 *  If random number modulo frequency is zero the sector is corrupted.
 *
 * Corruption Type: Sequential
 *  A count of sectors read/written is kept and every frequency sectors the
 *  sector is corrupted.
 **/

// Corruptor instance sysfs node
static struct kobject corruptorKobj;

/**********************************************************************/

#define DM_MSG_PREFIX  "corruptor"
#define SYSFS_DIR_NAME "corruptor"

#define MIN_IOS 64

typedef enum corruptionType {
  CORRUPTION_TYPE_NONE,
  CORRUPTION_TYPE_MODULO,
  CORRUPTION_TYPE_RANDOM,
  CORRUPTION_TYPE_SEQUENTIAL
} CorruptionType;

typedef struct corruptorDevice {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev        *dev;
  // The sysfs node that connects
  // /sys/<moduleName>/corruptor/<corruptorName> to this device.
  struct kobject        kobj;
  // The name of the device.
  // The name is located immediately after the allocated structure.
  char                 *corruptorName;
  // Pointer to the target's request queue
  struct request_queue *requestQueue;
  // Bio set used for cloning bios
  struct bio_set        bs;

  // Controls as to how sectors get corrupted and at what frequency.
  bool                  corruptRead;      // corrupt read sectors
  CorruptionType        readCorruption;   // type of corruption
  unsigned int          readFrequency;
  atomic_t              readSectors;      // counts sectors for sequential

  bool                  corruptWrite;     // corrupt write sectors
  CorruptionType        writeCorruption;  // type of corruption
  unsigned int          writeFrequency;
  atomic_t              writeSectors;     // counts sectors for sequential

  // BEGIN data that are merely statistics and do not effect code behavior.
  // These stats count the bios that arrive into the corruptorMap method.
  atomic64_t            readTotal;
  atomic64_t            writeTotal;
  atomic64_t            flushTotal;
  atomic64_t            fuaTotal;
  // END of statistics
} CorruptorDevice;

struct per_bio_data {
  struct bio *bioClone;
};

/**
 * Fills the sector buffer with random data and logs a block trace message
 * indicating that the sector has been corrupted.
 *
 * @param [in]  cd          corruptor device
 * @param [in]  read        if true, performing a read corruption; else write
 *                          corruption
 * @param [in]  sector      the sector being corrupted
 * @param [out] sectorBuf   the buffer containing the sector data; filled
 *                          with random bytes
 **/
static inline void corruptSector(CorruptorDevice *cd,
                                 bool             read,
                                 uint64_t         sector,
                                 char            *sectorBuf)
{
  get_random_bytes(sectorBuf, SECTOR_SIZE);
  blk_add_trace_msg(cd->requestQueue,
                    "%s %llu + 1 [pbit-corruptor, %s]",
                    read ? "CR" : "CW", sector, cd->corruptorName);
}

/**
 * Based on the configuration of the corruptor instance determines what form of
 * corruption, if any, should potentially be performed on the sectors of the
 * i/o.
 *
 * @param [in]  cd          corruptor device
 * @param [in]  bio         the i/o being performed
 * @param [in]  read        if true, performing a read corruption; else write
 *                          corruption
 **/
static void corruptSectors(CorruptorDevice *cd,
                           struct bio      *bio,
                           bool             read)
{
  // Get the corruption type
  CorruptionType corruptType = CORRUPTION_TYPE_NONE;
  if (read && cd->corruptRead) {
    corruptType = cd->readCorruption;
  } else if ((!read) && cd->corruptWrite) {
    corruptType = cd->writeCorruption;
  }

  // Return if we're not corrupting anything.
  if (corruptType == CORRUPTION_TYPE_NONE) {
    return;
  }

  // Grab info to help determine corruption.
  unsigned int frequency = read ? cd->readFrequency : cd->writeFrequency;
  atomic_t *atomic = read ? &cd->readSectors : &cd->writeSectors;

  // Iterate over the bio and corrupt sectors based on type.
  BioIterator iterator = createBioIterator(bio);
  BioVector *vector = getNextBiovec(&iterator);

  while (vector != NULL) {
    uint64_t       sector = vector->sector;
    uint64_t       sectorCount = to_sector(vector->bvec->bv_len);
    char          *data = bvec_kmap_local(vector->bvec);

    for (int i = 0; i < sectorCount; i++, sector++) {
      bool corrupt = false;
      unsigned int result;
      switch (corruptType) {
      case CORRUPTION_TYPE_MODULO:
        corrupt = ((sector % frequency) == 0);
        break;

      case CORRUPTION_TYPE_RANDOM:
        get_random_bytes(&result, sizeof(result));
        corrupt = ((result % frequency) == 0);
        break;

      case CORRUPTION_TYPE_SEQUENTIAL:
        result = atomic_add_return(1, atomic);
        corrupt = ((result % frequency) == 0);
        break;

      default:
        break;
      }

      if (corrupt) {
        corruptSector(cd, read, sector, data + (i * SECTOR_SIZE));
      }
    }

    kunmap_local(data);
    advanceBioIterator(&iterator);
    vector = getNextBiovec(&iterator);
  }
}

/**
 * This function returns a text string representing the
 * corruption type passed in.
 *
 * @param [in]  type          corruptor type
 */
static char* getCorruptionTypeString(CorruptionType type)
{
  switch (type) {
  case CORRUPTION_TYPE_MODULO:
    return "modulo";

  case CORRUPTION_TYPE_RANDOM:
    return "random";

  case CORRUPTION_TYPE_SEQUENTIAL:
    return "sequential";

  default:
    return "unknown";
  }
}

/**********************************************************************/
// BEGIN large section of code for the sysfs interface
/**********************************************************************/

typedef struct {
  struct attribute attr;
  ssize_t (*show)(CorruptorDevice *cd, char *buf);
  ssize_t (*store)(CorruptorDevice *cd, const char *value);
} CorruptorAttribute;

/**********************************************************************/
static void corruptorRelease(struct kobject *kobj)
{
  CorruptorDevice *cd = container_of(kobj, CorruptorDevice, kobj);
  kfree(cd);
}

/**********************************************************************/
static ssize_t corruptorShow(struct kobject   *kobj,
                             struct attribute *attr,
                             char             *buf)
{
  CorruptorDevice *cd = container_of(kobj, CorruptorDevice, kobj);
  CorruptorAttribute *ta = container_of(attr, CorruptorAttribute, attr);
  if (ta->show != NULL) {
    return ta->show(cd, buf);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t corruptorShowReadCorrupt(CorruptorDevice *cd, char *buf)
{
  strcpy(buf, cd->corruptRead ? "true\n" : "false\n");
  return strlen(buf);
}

/**********************************************************************/
static ssize_t corruptorShowReadFrequency(CorruptorDevice *cd, char *buf)
{
  return sprintf(buf, "%u\n", cd->readFrequency);
}

/**********************************************************************/
static ssize_t corruptorShowReadMode(CorruptorDevice *cd, char *buf)
{
  return sprintf(buf, "%s\n", getCorruptionTypeString(cd->readCorruption));
}

/**********************************************************************/
static ssize_t corruptorShowStatistics(CorruptorDevice *cd, char *buf)
{
  return sprintf(buf,
                 "reads: %lld\n"
                 "writes: %lld\n"
                 "flushes: %lld\n"
                 "FUAs: %lld\n",
                 (long long) atomic64_read(&cd->readTotal),
                 (long long) atomic64_read(&cd->writeTotal),
                 (long long) atomic64_read(&cd->flushTotal),
                 (long long) atomic64_read(&cd->fuaTotal));
}

/**********************************************************************/
static ssize_t corruptorShowWriteCorrupt(CorruptorDevice *cd, char *buf)
{
  strcpy(buf, cd->corruptWrite ? "true\n" : "false\n");
  return strlen(buf);
}

/**********************************************************************/
static ssize_t corruptorShowWriteFrequency(CorruptorDevice *cd, char *buf)
{
  return sprintf(buf, "%u\n", cd->writeFrequency);
}

/**********************************************************************/
static ssize_t corruptorShowWriteMode(CorruptorDevice *cd, char *buf)
{
  return sprintf(buf, "%s\n", getCorruptionTypeString(cd->writeCorruption));
}

/**********************************************************************/
static ssize_t corruptorStore(struct kobject   *kobj,
                              struct attribute *attr,
                              const char       *buf,
                              size_t            length)
{
  CorruptorDevice *cd = container_of(kobj, CorruptorDevice, kobj);
  CorruptorAttribute *ta = container_of(attr, CorruptorAttribute, attr);
  char *string = bufferToString(buf, length);
  ssize_t status;
  if (string == NULL) {
    status = -ENOMEM;
  } else if (ta->store != NULL) {
    status = ta->store(cd, string);
  } else {
    status = -EINVAL;
  }
  kfree(string);
  return status ? status : length;
}

/**********************************************************************/
static ssize_t corruptorStoreReadCorrupt(CorruptorDevice *cd,
                                         const char      *value)
{
  unsigned int val;
  if (sscanf(value, "%u", &val) != 1) {
    return -EINVAL;
  }

  if (val != 0) {
    atomic_set(&cd->readSectors, 0);
  }
  cd->corruptRead = val != 0;
  return 0;
}

/**********************************************************************/
static ssize_t corruptorStoreReadFrequency(CorruptorDevice *cd,
                                           const char      *value)
{
  unsigned int val;
  if ((sscanf(value, "%u", &val) != 1) || (val == 0)) {
    return -EINVAL;
  }

  atomic_set(&cd->readSectors, 0);
  cd->readFrequency = val;
  return 0;
}

/**********************************************************************/
static ssize_t corruptorStoreReadMode(CorruptorDevice *cd, const char *value)
{
  if (strncmp(value, "modulo", strlen("modulo")) == 0) {
    cd->readCorruption = CORRUPTION_TYPE_MODULO;
  } else if (strncmp(value, "random", strlen("random")) == 0) {
    cd->readCorruption = CORRUPTION_TYPE_RANDOM;
  } else if (strncmp(value, "sequential", strlen("sequential")) == 0) {
    atomic_set(&cd->readSectors, 0);
    cd->readCorruption = CORRUPTION_TYPE_SEQUENTIAL;
  } else {
    return -EINVAL;
  }
  return 0;
}

/**********************************************************************/
static ssize_t corruptorStoreWriteCorrupt(CorruptorDevice *cd,
                                          const char      *value)
{
  unsigned int val;
  if (sscanf(value, "%u", &val) != 1) {
    return -EINVAL;
  }

  if (val != 0) {
    atomic_set(&cd->writeSectors, 0);
  }
  cd->corruptWrite = val != 0;
  return 0;
}

/**********************************************************************/
static ssize_t corruptorStoreWriteFrequency(CorruptorDevice *cd,
                                            const char      *value)
{
  unsigned int val;
  if ((sscanf(value, "%u", &val) != 1) || (val == 0)) {
    return -EINVAL;
  }

  atomic_set(&cd->writeSectors, 0);
  cd->writeFrequency = val;
  return 0;
}

/**********************************************************************/
static ssize_t corruptorStoreWriteMode(CorruptorDevice *cd, const char *value)
{
  if (strncmp(value, "modulo", strlen("modulo")) == 0) {
    cd->writeCorruption = CORRUPTION_TYPE_MODULO;
  } else if (strncmp(value, "random", strlen("random")) == 0) {
    cd->writeCorruption = CORRUPTION_TYPE_RANDOM;
  } else if (strncmp(value, "sequential", strlen("sequential")) == 0) {
    atomic_set(&cd->readSectors, 0);
    cd->writeCorruption = CORRUPTION_TYPE_SEQUENTIAL;
  } else {
    return -EINVAL;
  }
  return 0;
}

/**********************************************************************/

static CorruptorAttribute readCorruptAttr = {
  .attr = { .name = "readCorrupt", .mode = 0644 },
  .show = corruptorShowReadCorrupt,
  .store = corruptorStoreReadCorrupt,
};

static CorruptorAttribute readFrequencyAttr = {
  .attr = { .name = "readFrequency", .mode = 0644 },
  .show = corruptorShowReadFrequency,
  .store = corruptorStoreReadFrequency,
};

static CorruptorAttribute readModeAttr = {
  .attr = { .name = "readMode", .mode = 0644 },
  .show = corruptorShowReadMode,
  .store = corruptorStoreReadMode,
};

static CorruptorAttribute statisticsAttr = {
  .attr = { .name = "statistics", .mode = 0444 },
  .show = corruptorShowStatistics,
};

static CorruptorAttribute writeCorruptAttr = {
  .attr = { .name = "writeCorrupt", .mode = 0644 },
  .show = corruptorShowWriteCorrupt,
  .store = corruptorStoreWriteCorrupt,
};

static CorruptorAttribute writeFrequencyAttr = {
  .attr = { .name = "writeFrequency", .mode = 0644 },
  .show = corruptorShowWriteFrequency,
  .store = corruptorStoreWriteFrequency,
};

static CorruptorAttribute writeModeAttr = {
  .attr = { .name = "writeMode", .mode = 0644 },
  .show = corruptorShowWriteMode,
  .store = corruptorStoreWriteMode,
};

static struct attribute *corruptor_attrs[] = {
  &readCorruptAttr.attr,
  &readFrequencyAttr.attr,
  &readModeAttr.attr,
  &statisticsAttr.attr,
  &writeCorruptAttr.attr,
  &writeFrequencyAttr.attr,
  &writeModeAttr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(corruptor);

static struct sysfs_ops corruptorOps = {
  .show  = corruptorShow,
  .store = corruptorStore,
};

static struct kobj_type corruptorObjectType = {
  .release        = corruptorRelease,
  .sysfs_ops      = &corruptorOps,
  .default_groups = corruptor_groups,
};

/**********************************************************************/
// BEGIN device methods for the corruptor target type
/**********************************************************************/

/**********************************************************************/
static int corruptorCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 2) {
    ti->error = "requires exactly 2 arguments";
    return -EINVAL;
  }
  const char *corruptorName       = argv[0];
  int         corruptorNameLength = strlen(corruptorName) + 1;
  const char *devicePath          = argv[1];

  CorruptorDevice *cd = kzalloc(sizeof(CorruptorDevice) + corruptorNameLength,
                                GFP_KERNEL);
  if (cd == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }

  cd->corruptorName = ((char *) cd) + sizeof(CorruptorDevice);
  strncpy(cd->corruptorName, corruptorName, corruptorNameLength);

  if (dmGetDevice(ti, devicePath, &cd->dev)) {
    ti->error = "Device lookup failed";
    kfree(cd);
    return -EINVAL;
  }

  cd->corruptRead     = false;
  cd->readCorruption  = CORRUPTION_TYPE_RANDOM;
  cd->readFrequency   = 1;
  atomic_set(&cd->readSectors, 0);

  cd->corruptWrite    = false;
  cd->writeCorruption = CORRUPTION_TYPE_RANDOM;
  cd->writeFrequency  = 1;
  atomic_set(&cd->writeSectors, 0);

  kobject_init(&cd->kobj, &corruptorObjectType);
  int result = kobject_add(&cd->kobj, &corruptorKobj, cd->corruptorName);
  if (result < 0) {
    ti->error = "sysfs addition failed";
    dm_put_device(ti, cd->dev);
    kfree(cd);
    return result;
  }

  result = bioset_init(&cd->bs, MIN_IOS, 0, BIOSET_NEED_BVECS);
  if (result < 0) {
    ti->error = "Cannot allocate corruptor bioset";
    dm_put_device(ti, cd->dev);
    kobject_put(&cd->kobj);
    kfree(cd);
    return result;
  }

  ti->flush_supported = 1;
  ti->num_flush_bios = 1;
  ti->per_io_data_size = sizeof(struct per_bio_data);
  ti->private = cd;
  return 0;
}

/**********************************************************************/
static void corruptorDtr(struct dm_target *ti)
{
  CorruptorDevice *cd = ti->private;
  dm_put_device(ti, cd->dev);
  bioset_exit(&cd->bs);
  kobject_put(&cd->kobj);
}

/**********************************************************************/
static int corruptorEndIo(struct dm_target *ti,
                          struct bio       *bio,
                          BioStatusType    *error)
{
  CorruptorDevice *cd = ti->private;

  struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
  bio = pb->bioClone;

  bool doWork = false;
  doWork = ((*error == BIO_SUCCESS) && isReadBio(bio));
  if (doWork) {
    corruptSectors(cd, bio, true);
  }

  // Release the clone.
  bio_put(bio);

  return blk_status_to_errno(*error);
}

/**********************************************************************/
static int corruptorMap(struct dm_target *ti,
                        struct bio       *bio)
{
  CorruptorDevice *cd = ti->private;

  // If we don't yet have the request queue (necessary for logging bio info)
  // associated with the device of this corruptor instance get it via the bio.
  if (cd->requestQueue == NULL) {
    cd->requestQueue = bdev_get_queue(bio->bi_bdev);
  }

  // Map the I/O to the storage device.
  setBioBlockDevice(bio, cd->dev->bdev);
  setBioSector(bio, dm_target_offset(ti, getBioSector(bio)));

  // Get a clone of the original bio for any necessary end io processing.
  struct bio *bioClone = cloneBio(bio, &cd->bs);
  if (bioClone == NULL) {
    printk("failure to clone bio");
    return -ENOMEM;
  }
  struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
  pb->bioClone = bioClone;

  // Perform accounting.
  if (bio_data_dir(bio) == READ) {
    atomic64_inc(&cd->readTotal);
  } else {
    if (isFlushBio(bio)) {
      atomic64_inc(&cd->flushTotal);
    }
    if (isFUABio(bio)) {
      atomic64_inc(&cd->fuaTotal);
    }
    if (getBioSize(bio) > 0) {
      atomic64_inc(&cd->writeTotal);
    }
  }

  if (isWriteBio(bio)) {
    corruptSectors(cd, bio, false);
  }

  return DM_MAPIO_REMAPPED;
}

/**********************************************************************/
static int corruptorMessage(struct dm_target  *ti,
                            unsigned int       argc,
                            char             **argv,
                            char              *resultBuffer,
                            unsigned int       maxlen)
{
  CorruptorDevice *cd = ti->private;
  bool        invalidMessage = false;

  int result = 0;

  if (isGlobalDisableMessage(argc, argv)) {
    cd->corruptRead = false;
    cd->corruptWrite = false;
  } else if (isGlobalEnableMessage(argc, argv)) {
    cd->corruptRead = true;
    cd->corruptWrite = true;
  } else if ((argc == 2) && isArgString(argv[0], "disable")) {
    bool  disableRead = isArgString(argv[1], "read");
    bool  disableWrite = isArgString(argv[1], "write");

    invalidMessage = (!disableRead) && (!disableWrite);
    if (!invalidMessage) {
      if (disableRead) {
        cd->corruptRead = false;
      }
      if (disableWrite) {
        cd->corruptWrite = false;
      }
    }
  } else if ((argc == 2) && isArgString(argv[0], "enable")) {
    bool  enableRead = isArgString(argv[1], "read");;
    bool  enableWrite = isArgString(argv[1], "write");;

    invalidMessage = (!enableRead) && (!enableWrite);
    if (!invalidMessage) {
      if (enableRead) {
        cd->corruptRead = true;
      }
      if (enableWrite) {
        cd->corruptWrite = true;
      }
    }
  } else if ((argc == 4) &&
              (isArgString(argv[0], "enable")
                || isArgString(argv[0], "parameters"))) {
    invalidMessage
      = ((!isArgString(argv[1], "read")) && (!isArgString(argv[1], "write")))
          || ((!isArgString(argv[2], "modulo"))
              && (!isArgString(argv[2], "random"))
              && (!isArgString(argv[2], "sequential")));

    unsigned int frequency = 0;
    if (!invalidMessage) {
      invalidMessage
        = (sscanf(argv[3], "%u", &frequency) != 1) || (frequency == 0);
    }

    if (!invalidMessage) {
      if (isArgString(argv[1], "read")) {
        if (isArgString(argv[2], "modulo")) {
          cd->readCorruption = CORRUPTION_TYPE_MODULO;
        } else if (isArgString(argv[2], "random")) {
          cd->readCorruption = CORRUPTION_TYPE_RANDOM;
        } else {
          cd->readCorruption = CORRUPTION_TYPE_SEQUENTIAL;
        }
        cd->readFrequency = frequency;
        atomic_set(&cd->readSectors, 0);
        cd->corruptRead = cd->corruptRead || isArgString(argv[0], "enable");
      } else {
        if (isArgString(argv[2], "modulo")) {
          cd->writeCorruption = CORRUPTION_TYPE_MODULO;
        } else if (isArgString(argv[2], "random")) {
          cd->writeCorruption = CORRUPTION_TYPE_RANDOM;
        } else {
          cd->writeCorruption = CORRUPTION_TYPE_SEQUENTIAL;
        }
        cd->writeFrequency = frequency;
        atomic_set(&cd->writeSectors, 0);
        cd->corruptWrite = cd->corruptWrite || isArgString(argv[0], "enable");
      }
    }
  } else {
    invalidMessage = true;
  }

  if (invalidMessage) {
    result = -EINVAL;
    printk("unrecognized dmsetup message '%s' received\n", argv[0]);
  }

  return result;
}

/**********************************************************************/
static void corruptorStatus(struct dm_target *ti,
                            status_type_t     type,
                            unsigned int      status_flags,
                            char             *result,
                            unsigned int      maxlen)
{
  CorruptorDevice *cd = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro

  switch (type) {
  case STATUSTYPE_INFO:
    DMEMIT("%s /dev/%pg read %s %s %u write %s %s %u",
	   cd->corruptorName,
           cd->dev->bdev,
	   cd->corruptRead ? "on" : "off",
	   getCorruptionTypeString(cd->readCorruption),
	   cd->readFrequency,
	   cd->corruptWrite ? "on" : "off",
	   getCorruptionTypeString(cd->writeCorruption),
	   cd->writeFrequency);
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s /dev/%pg", cd->corruptorName, cd->dev->bdev);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type corruptorTargetType = {
  .name            = "corruptor",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = corruptorCtr,
  .dtr             = corruptorDtr,
  .end_io          = corruptorEndIo,
  .iterate_devices = commonIterateDevices,
  .map             = corruptorMap,
  .message         = corruptorMessage,
  .status          = corruptorStatus,
  // Put version specific functions at the bottom
  .prepare_ioctl   = commonPrepareIoctl,
};

/**********************************************************************/
int __init corruptorInit(void)
{
  STATIC_ASSERT(offsetof(CorruptorDevice, dev)
                == offsetof(CommonDevice, dev));

  kobject_init(&corruptorKobj, &emptyObjectType);
  int result = kobject_add(&corruptorKobj, NULL, THIS_MODULE->name);
  if (result < 0) {
    return result;
  }

  result = dm_register_target(&corruptorTargetType);
  if (result < 0) {
    kobject_put(&corruptorKobj);
    DMERR("dm_register_target failed %d", result);
  }
  return result;
}

/**********************************************************************/
void __exit corruptorExit(void)
{
  dm_unregister_target(&corruptorTargetType);
  kobject_put(&corruptorKobj);
}

module_init(corruptorInit);
module_exit(corruptorExit);

MODULE_DESCRIPTION(DM_NAME " corrupting test device");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
