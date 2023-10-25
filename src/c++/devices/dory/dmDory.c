/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * This is the test "Dory" device, which has a short term memory problem.
 * It has these expected usage modes:
 *
 * 1 - No cache, device stops suddenly.
 *
 *     There is no cache.  At a point chosen by the test, we suddenly start
 *     failing all writes with an EIO.
 *
 * 2 - There is a cache of 4K blocks.  The device obeys proper REQ_FLUSH and
 *     REQ_FUA semantics.  At a point chosen by the test, we suddenly start
 *     failing all writes with an EIO, and forget to write the contents of the
 *     write cache.
 *
 *     The cache is not managed to improve performance or reliability, but
 *     merely provides data that we forget to write.
 *
 *     The cache size can be large or small, which determines the size of the
 *     disruption caused by the device failure.
 *
 * 3 - There is a cache of 512 byte blocks (sectors).  The device obeys proper
 *     REQ_FLUSH and REQ_FUA semantics.  At a point chosen by the test, we
 *     suddenly start failing all writes with an EIO, and forget to write the
 *     contents of the write cache.
 *
 *     We do not cache every sector, but select which sectors to cache so as to
 *     produce torn writes when we stop the device.  We use a modulus and mask
 *     to decide with sectors to cache.  Specifically, we cache a sector when
 *     this expression evaluates to a true value:
 *
 *          mask & (1 << (sector_number % modulus))
 *
 *     Using modulus of 8 with a mask with only 1 bit set will cache only 1
 *     sector of a 4K block and will cause the Dory device to fail to write
 *     that sector.  Using modulus of 8 with a mask with only 1 bit clear will
 *     cache all but 1 sector of a 4K block, and will cause the Dory device to
 *     write only 1 sector of the block.
 *
 *     A more interesting effect happens with a modulus of 9.  Similar mask
 *     settings will result in the sector that is/isn't written to change to a
 *     different offset in each 4K block.
 *
 * $Id$
 */

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "common.h"
#include "dmDory.h"

static struct kobject doryKobj;

/**********************************************************************/

#define DM_MSG_PREFIX  "dory"
#define SYSFS_DIR_NAME "dory"

enum { DORY_NAME_SIZE = 11 };

/*
 * Cache block states.  Note that all state changes are protected by a spin
 * lock.  The states are:
 *
 *   EMPTY    The cache block is not used and is available.
 *
 *   COPYING  There is an active bio doing a read or write to the cache block.
 *
 *   DIRTY    The cache block is in use, but there is no active I/O on it.
 *
 *   WRITING  The cache block is being written to storage.
 *
 * This driver intends to be correct until it is told to stop doing any writing
 * to storage.  It sometimes prefers to be simple rather than fast.  These are
 * the state transitions that it performs:
 *
 *   EMPTY => COPYING => DIRTY
 *
 *     This transition takes a cache block from unused to used.  We only do
 *     this for an ordinary write of a full block.  This means the "copying"
 *     copies a full block from the I/O request into the cache, and ensures
 *     that there are no partial blocks in the cache.
 *
 *   DIRTY => COPYING => DIRTY
 *
 *     This transition services an I/O request using the cache block.
 *
 *   DIRTY => WRITING => EMPTY
 *
 *     This transition writes the cache block to storage.  It can occur
 *     when an empty REQ_FLUSH request is being processed, or when a write
 *     request to this block is either an REQ_DISCARD or REQ_FUA request.
 *     When the write completes, the cache block returns to EMPTY state.
 *     We do not try to maintain any "clean" blocks in the cache.
 */
typedef enum __attribute__((__packed__)) {
  EMPTY,
  COPYING,
  DIRTY,
  WRITING,
} BlockState;

typedef struct {
  // A spin lock that protects the cache block.  It is taken by the bi_end_io
  // callback when we write a cache block, and therefore should be used with
  // spin_lock_irq or spin_lock_irqsave.
  spinlock_t         lock;
  // When this block is in COPYING or WRITING state, bios that refer to this
  // block are put on this list and processed later.
  struct bio_list    waitingBios;
  // Pointer back to the DoryDevice containing this block.
  struct doryDevice *doryDevice;
  // Pointer to the data for this block.
  char              *blockData;
  // Pointer to the bio reserved for use when we need to write this block.
  struct bio        *blockBio;
  // The BLOCK number of this block (not the sector number).
  sector_t           blockNumber;
  // The state of this cache block
  BlockState         state;
} CacheBlock;

typedef struct doryDevice {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev  *dev;
  // The sysfs node that connects /sys/<moduleName>/dory/<doryName> to this
  // dory device.
  struct kobject  kobj;
  // Return value for unsuccessful writes.
  BioStatusType   ioError;
  // Flag that is set to true to stop all writes by the device.
  bool            stopFlag;
  // The name of the Dory device.
  char            doryName[DORY_NAME_SIZE + 1];
  // Pointer to the cached data, used only for allocate/free of the memory.
  char           *cacheData;
  // The block size, which must be either 512 or 4K.
  size_t          blockSize;
  // Settings for producing torn writes.
  unsigned int    tornMask;
  unsigned int    tornModulus;
  // The block shift, which is used to convert sector numbers to block numbers.
  // Will be either 0 (for blockSize 512) or 3 (for blockSize 4K).
  unsigned int    blockShift;
  // The number of cache blocks, which may be zero for no block cache.
  unsigned int    cacheBlockCount;
  // The busy count of the device, which is used to implement proper REQ_FLUSH
  // requests when there is a block cache.  It counts the number of bios that
  // we are actively working on, and the number of dirty blocks in the block
  // cache.  An REQ_FLUSH request cannot be completed until this count goes to
  // zero.
  atomic_t        busyCount;

  // BEGIN data that pertains to work done in a kworker thread for this Dory
  // device.  This spin lock protects these data, and it is taken by the
  // bi_end_io callback when we write a cache block, and therefore should be
  // used with spin_lock_irq or spin_lock_irqsave.
  spinlock_t         workLock;
  // When the processing of a bio has been delayed, it will eventually be put
  // on this list and processed in a kworker thread.
  struct bio_list    workBios;
  // When the processing of an REQ_FLUSH request has been completed, it will be
  // put on this list and processed in a kworker thread.
  struct bio_list    workFlushBios;
  // This is a Linux work item used to schedule processing of the workBios
  // list.
  struct work_struct workWork;
  // END of data protected by workLock.

  // BEGIN data that pertains to processing REQ_FLUSH requests.  This spin lock
  // protects these data, and may be taken by the bi_end_io callback when we
  // write a cache block, and therefore should be used with spin_lock_irq or
  // spin_lock_irqsave.
  spinlock_t      flushLock;
  // A flag to indicate that a flush is in progress.
  bool            flushFlag;
  // When an REQ_FLUSH bio arrives, it will be put onto this list for
  // processing at the proper time.
  struct bio_list flushBios;
  // When flushFlag is set, all non-REQ_FLUSH bios are put onto this list for
  // processing when the flush is completed.
  struct bio_list waitingBios;
  // END of data protected by flushLock.

  // BEGIN data that are merely statistics and do not effect code behavior.
  // These stats count the bios that arrive into the doryMap method.
  atomic64_t    readTotal;
  atomic64_t    writeTotal;
  atomic64_t    flushTotal;
  atomic64_t    fuaTotal;
  atomic64_t    writeFailure;
  atomic64_t    flushFailure;
  unsigned long readsAtLastFlush;
  unsigned long writesAtLastFlush;
  unsigned long readsAtStop;
  unsigned long writesAtStop;
  // These stats count the values returned by the doryMap method.
  atomic64_t    mappedReturns;
  atomic64_t    submittedReturns;
  // These stats count the bios for which the doryMap method returned
  // "submitted".
  atomic64_t    submittedBios;
  atomic64_t    successBios;
  atomic64_t    errorBios;
  // END of statistics

  // The block cache (variable sized, so it goes at the end).
  CacheBlock cacheBlocks[];
} DoryDevice;

static void processBioList(DoryDevice *dd, struct bio_list *ready);

/**********************************************************************/
// BEGIN large section of code for the sysfs interface
/**********************************************************************/

typedef struct {
  struct attribute attr;
  ssize_t (*show)(DoryDevice *dd, char *buf);
  ssize_t (*store)(DoryDevice *dd, const char *value);
} DoryAttribute;

/**********************************************************************/
static void doryRelease(struct kobject *kobj)
{
  DoryDevice *dd = container_of(kobj, DoryDevice, kobj);
  kfree(dd);
}

/**********************************************************************/
static ssize_t doryShow(struct kobject   *kobj,
                        struct attribute *attr,
                        char             *buf)
{
  DoryDevice *dd = container_of(kobj, DoryDevice, kobj);
  DoryAttribute *da = container_of(attr, DoryAttribute, attr);
  if (da->show != NULL) {
    return da->show(dd, buf);
  }
  return -EINVAL;
}

/**********************************************************************/
static ssize_t doryShowCache(DoryDevice *dd, char *buf)
{
  // The string that indicates the data do not fit in the output.
  static const char ETC_STRING[] = "...\n";
  static const int ETC_LENGTH = sizeof(ETC_STRING) - 1;
  // The maximum length of an output line
  //               %u      %s      %u       %u  \n
  enum { LINE_MAX = 5 + 1 + 7 + 1 + 2 + 1 + 12 + 1 };

  bool full = false;
  int length = 0;
  for (unsigned int i = 0; !full && (i < dd->cacheBlockCount); i++) {
    CacheBlock *cb = &dd->cacheBlocks[i];
    spin_lock_irq(&cb->lock);
    unsigned int waiterCount = bio_list_size(&cb->waitingBios);
    sector_t     sector      = cb->blockNumber << dd->blockShift;
    BlockState   blockState  = cb->state;
    spin_unlock_irq(&cb->lock);
    char *state = "UNKNOWN";
    switch (blockState) {
    case EMPTY:                       continue;
    case COPYING:  state = "COPYING"; break;
    case DIRTY:    state = "DIRTY";   break;
    case WRITING:  state = "WRITING"; break;
    default:                          break;
    }
    full = (length > PAGE_SIZE - LINE_MAX - ETC_LENGTH - 1);
    if (!full) {
      char *line = &buf[length];
      snprintf(line, LINE_MAX, "%u %s %u %llu\n", i, state, waiterCount,
               (unsigned long long) sector);
      length += strlen(line);
    }
  }
  if (full) {
    strcpy(&buf[length], ETC_STRING);
    length += ETC_LENGTH;
  }
  return length;
}

/**********************************************************************/
static ssize_t doryShowMode(DoryDevice *dd, char *buf)
{
  strcpy(buf, dd->stopFlag ? "stop\n" : "running\n");
  return strlen(buf);
}

/**********************************************************************/
static ssize_t doryShowState(DoryDevice *dd, char *buf)
{
  spin_lock_irq(&dd->flushLock);
  unsigned int flushFlushCount = bio_list_size(&dd->flushBios);
  unsigned int flushBioCount   = bio_list_size(&dd->waitingBios);
  spin_unlock_irq(&dd->flushLock);
  spin_lock_irq(&dd->workLock);
  unsigned int workFlushCount = bio_list_size(&dd->workFlushBios);
  unsigned int workBioCount   = bio_list_size(&dd->workBios);
  spin_unlock_irq(&dd->workLock);
  return sprintf(buf,
                 "blockSize: %zu\n"
                 "cacheBlockCount: %u\n"
                 "tornMask: %u\n"
                 "tornModulus: %u\n"
                 "busyCount: %d\n"
                 "stopFlag: %u\n"
                 "flushFlag: %u\n"
                 "flushFlushCount: %u\n"
                 "flushBioCount: %u\n"
                 "workFlushCount: %u\n"
                 "workBioCount: %u\n",
                 dd->blockSize,
                 dd->cacheBlockCount,
                 dd->tornMask,
                 dd->tornModulus,
                 atomic_read(&dd->busyCount),
                 dd->stopFlag,
                 dd->flushFlag,
                 flushFlushCount,
                 flushBioCount,
                 workFlushCount,
                 workBioCount);
}

/**********************************************************************/
static ssize_t doryShowStatistics(DoryDevice *dd, char *buf)
{
  return sprintf(buf,
                 "reads: %lld\n"
                 "writes: %lld\n"
                 "flushes: %lld\n"
                 "FUAs: %lld\n"
                 "writeFailure: %lld\n"
                 "flushFailure: %lld\n"
                 "readsAtLastFlush: %lu\n"
                 "writesAtLastFlush: %lu\n"
                 "readsAtStop: %lu\n"
                 "writesAtStop: %lu\n"
                 "mappedReturns: %lld\n"
                 "submittedReturns: %lld\n"
                 "submittedBios: %lld\n"
                 "successBios: %lld\n"
                 "errorBios: %lld\n",
                 (long long) atomic64_read(&dd->readTotal),
                 (long long) atomic64_read(&dd->writeTotal),
                 (long long) atomic64_read(&dd->flushTotal),
                 (long long) atomic64_read(&dd->fuaTotal),
                 (long long) atomic64_read(&dd->writeFailure),
                 (long long) atomic64_read(&dd->flushFailure),
                 dd->readsAtLastFlush,
                 dd->writesAtLastFlush,
                 dd->readsAtStop,
                 dd->writesAtStop,
                 (long long) atomic64_read(&dd->mappedReturns),
                 (long long) atomic64_read(&dd->submittedReturns),
                 (long long) atomic64_read(&dd->submittedBios),
                 (long long) atomic64_read(&dd->successBios),
                 (long long) atomic64_read(&dd->errorBios));
}

/**********************************************************************/
static ssize_t doryShowTornMask(DoryDevice *dd, char *buf)
{
  return sprintf(buf, "%u\n", dd->tornMask);
}

/**********************************************************************/
static ssize_t doryShowTornModulus(DoryDevice *dd, char *buf)
{
  return sprintf(buf, "%u\n", dd->tornModulus);
}

/**********************************************************************/
static ssize_t doryStore(struct kobject   *kobj,
                         struct attribute *attr,
                         const char       *buf,
                         size_t            length)
{
  DoryDevice *dd = container_of(kobj, DoryDevice, kobj);
  DoryAttribute *da = container_of(attr, DoryAttribute, attr);
  char *string = bufferToString(buf, length);
  ssize_t status;
  if (string == NULL) {
    status = -ENOMEM;
  } else if (da->store != NULL) {
    status = da->store(dd, string);
  } else {
    status = -EINVAL;
  }
  kfree(string);
  return status ? status : length;
}

/**********************************************************************/
static ssize_t doryStoreStop(DoryDevice *dd, const char *value)
{
  dd->stopFlag = true;
  dd->readsAtStop  = atomic64_read(&dd->readTotal);
  dd->writesAtStop = atomic64_read(&dd->writeTotal);
  return 0;
}

/**********************************************************************/
static ssize_t doryStoreReturnEIO(DoryDevice *dd, const char *value)
{
  unsigned int val;
  if (sscanf(value, "%u", &val) != 1) {
    return -EINVAL;
  }
  if (val == 0) {
    dd->ioError = BIO_SUCCESS;
  } else if (val == 1) {
    dd->ioError = BIO_EIO;
  } else {
    return -EINVAL;
  }
  return 0;
}

/**********************************************************************/
static ssize_t doryStoreTornMask(DoryDevice *dd, const char *value)
{
  unsigned long m;
  if (sscanf(value, "%lu", &m) != 1) {
    return -EINVAL;
  }
  if (m == 0) {
    return -EINVAL;
  }
  dd->tornMask = m;
  return 0;
}

/**********************************************************************/
static ssize_t doryStoreTornModulus(DoryDevice *dd, const char *value)
{
  unsigned long m;
  if (sscanf(value, "%lu", &m) != 1) {
    return -EINVAL;
  }
  if ((m < 8) || (m > 32)) {
    return -EINVAL;
  }
  dd->tornModulus = m;
  return 0;
}

/**********************************************************************/

static DoryAttribute cacheAttr = {
  .attr = { .name = "cache", .mode = 0444 },
  .show = doryShowCache,
};

static DoryAttribute modeAttr = {
  .attr = { .name = "mode", .mode = 0444 },
  .show = doryShowMode,
};

static DoryAttribute returnEIOAttr = {
  .attr  = { .name = "returnEIO", .mode = 0200 },
  .store = doryStoreReturnEIO,
};

static DoryAttribute stateAttr = {
  .attr = { .name = "state", .mode = 0444 },
  .show = doryShowState,
};

static DoryAttribute statisticsAttr = {
  .attr = { .name = "statistics", .mode = 0444 },
  .show = doryShowStatistics,
};

static DoryAttribute stopAttr = {
  .attr  = { .name = "stop", .mode = 0200 },
  .store = doryStoreStop,
};

static DoryAttribute tornMaskAttr = {
  .attr  = { .name = "torn_mask", .mode = 0644 },
  .show  = doryShowTornMask,
  .store = doryStoreTornMask,
};

static DoryAttribute tornModulusAttr = {
  .attr  = { .name = "torn_modulus", .mode = 0644 },
  .show  = doryShowTornModulus,
  .store = doryStoreTornModulus,
};

static struct attribute *dory_attrs[] = {
  &cacheAttr.attr,
  &modeAttr.attr,
  &returnEIOAttr.attr,
  &stateAttr.attr,
  &statisticsAttr.attr,
  &stopAttr.attr,
  &tornMaskAttr.attr,
  &tornModulusAttr.attr,
  NULL,
};
ATTRIBUTE_GROUPS(dory);

static struct sysfs_ops doryOps = {
  .show  = doryShow,
  .store = doryStore,
};

static struct kobj_type doryObjectType = {
  .release        = doryRelease,
  .sysfs_ops      = &doryOps,
  .default_groups = dory_groups,
};

/**********************************************************************/
// BEGIN large section of code for the block cache
/**********************************************************************/

/**
 * Do delayed processing of a list of bios in a kworker thread.
 *
 * @param work  A kworker work struct.
 **/
static void processDelayed(struct work_struct *work)
{
  DoryDevice *dd = container_of(work, DoryDevice, workWork);

  // Under the worklock, grab the lists of bios to be processed.
  struct bio_list flushes, ready;
  bio_list_init(&flushes);
  bio_list_init(&ready);
  spin_lock_irq(&dd->workLock);
  bio_list_merge(&flushes, &dd->workFlushBios);
  bio_list_init(&dd->workFlushBios);
  bio_list_merge(&ready, &dd->workBios);
  bio_list_init(&dd->workBios);
  spin_unlock_irq(&dd->workLock);

  // Process the completed flushes.
  struct bio *bio;
  while ((bio = bio_list_pop(&flushes)) != NULL) {
    if (dd->stopFlag && (atomic64_read(&dd->writeFailure) > 0)) {
      // We are stopping writes and failed to write a cached block.
      endio(bio, dd->ioError);
      atomic64_inc(&dd->errorBios);
    } else {
      // Still succeeding, so forward the flush to the storage medium.
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
      submit_bio_noacct(bio);
#else
      dm_submit_bio_remap(bio, NULL);
#endif
      atomic64_inc(&dd->submittedBios);
    }
  }

  // Process the delayed bios.
  processBioList(dd, &ready);
}

/**********************************************************************/
/**
 * Schedule delayed processing of bios.  This uses the Linux kworker
 * threads, so as to avoid extended processing in a bi_end_io callback.
 *
 * @param dd       The Dory device
 * @param ready    bio list of bios that are now unblocked
 * @param flushes  bio list of REQ_FLUSH bios that are now complete
 **/
static void scheduleDelayedProcessing(DoryDevice      *dd,
                                      struct bio_list *ready,
                                      struct bio_list *flushes)
{
  bool haveBios = !bio_list_empty(ready);
  bool haveFlushes = (flushes != NULL) && !bio_list_empty(flushes);

  // If the lists of new bios are empty, there is nothing to do.
  if (!haveFlushes && !haveBios) {
    return;
  }

  // Under the worklock, add the new bios to the existing lists of bios to
  // process.
  unsigned long flags;
  spin_lock_irqsave(&dd->workLock, flags);
  bool schedulingNeeded = (bio_list_empty(&dd->workBios)
                           && bio_list_empty(&dd->workFlushBios));
  if (haveBios) {
    bio_list_merge(&dd->workBios, ready);
    bio_list_init(ready);
  }
  if (haveFlushes) {
    bio_list_merge(&dd->workFlushBios, flushes);
    bio_list_init(flushes);
  }
  spin_unlock_irqrestore(&dd->workLock, flags);

  // If we added to empty lists, schedule a work item.  Otherwise there is
  // already a work item scheduled.
  if (schedulingNeeded) {
    INIT_WORK(&dd->workWork, processDelayed);
    schedule_work(&dd->workWork);
  }
}

/**********************************************************************/
/**
 * Decrement the busy count.  If it goes to zero and a flush is in progress,
 * finish the flush.  This method can be called from a bi_end_io callback.
 *
 * @param dd  The Dory device
 **/
static void decrementBusyCountAndTest(DoryDevice *dd)
{
  if (atomic_dec_and_test(&dd->busyCount)) {
    // The busy count has just dropped to zero, so we need to take flushLock
    // and deal with any flushes in progress.
    struct bio_list completedFlushes, readyBios;
    unsigned long flags;
    bio_list_init(&completedFlushes);
    bio_list_init(&readyBios);
    spin_lock_irqsave(&dd->flushLock, flags);
    if (dd->flushFlag) {
      // And there are REQ_FLUSH requests in progress.
      dd->flushFlag = false;
      // Record the flush bios that are complete.
      bio_list_merge(&completedFlushes, &dd->flushBios);
      bio_list_init(&dd->flushBios);
      // Record the bios that are now ready to start.
      bio_list_merge(&readyBios, &dd->waitingBios);
      bio_list_init(&dd->waitingBios);
    }
    spin_unlock_irqrestore(&dd->flushLock, flags);

    // Start the "ready" ones.
    scheduleDelayedProcessing(dd, &readyBios, &completedFlushes);
  }
}

/**********************************************************************/
/**
 * bi_end_io callback routine for when a cache block write completes
 *
 * @param bio    The bio
 **/
static void endFlushCacheBlock(struct bio *bio)
{
  int error = getBioResult(bio);
  CacheBlock *cb = bio->bi_private;
  DoryDevice *dd = cb->doryDevice;
  struct bio_list ready;
  bio_list_init(&ready);

  if (error) {
    printk(KERN_WARNING "error flushing at sector %llu: %d\n",
           (unsigned long long) (cb->blockNumber << dd->blockShift), error);
  }

  // Set the block state to EMPTY.  This is a transition from WRITING to EMPTY.
  unsigned long flags;
  spin_lock_irqsave(&cb->lock, flags);
  cb->state = EMPTY;
  // Record the bios that are now ready to start
  bio_list_merge(&ready, &cb->waitingBios);
  bio_list_init(&cb->waitingBios);
  spin_unlock_irqrestore(&cb->lock, flags);

  // Finish the transition to EMPTY.
  decrementBusyCountAndTest(dd);

  // Start any bios that were waiting for this specific cache block.
  scheduleDelayedProcessing(dd, &ready, NULL);
}

/**********************************************************************/
/**
 * Flush a cache block to storage.
 *
 * @param cb  The cache block (locked)
 **/
static void flushCacheBlock(CacheBlock *cb)
{
  DoryDevice *dd = cb->doryDevice;

  // Set the block state to WRITING, and release the cache block lock.  We do
  // not want to hold the lock while we write the data.
  cb->state = WRITING;
  spin_unlock_irq(&cb->lock);

  // Start writing the cache block
#undef USE_ALTERNATE
#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0))
#endif
#if USE_ALTERNATE
  bio_reset(cb->blockBio);
  cb->blockBio->bi_opf = REQ_OP_WRITE;
#else
  bio_reset(cb->blockBio, dd->dev->bdev, REQ_OP_WRITE);
#endif
  cb->blockBio->bi_end_io  = endFlushCacheBlock;
  cb->blockBio->bi_private = cb;
  setBioBlockDevice(cb->blockBio, dd->dev->bdev);
  setBioSector(cb->blockBio, cb->blockNumber << dd->blockShift);
  int bytes_added =
    bio_add_page(cb->blockBio, vmalloc_to_page(cb->blockData), dd->blockSize,
                 (unsigned long) cb->blockData % PAGE_SIZE);
  if (bytes_added != dd->blockSize) {
    printk(KERN_WARNING "problem adding block data to bio");
  }
  if (dd->stopFlag) {
    // We are supposed to stop writing, so fail the write.
    atomic64_inc(&dd->flushFailure);
    endio(cb->blockBio, dd->ioError);
  } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
    submit_bio_noacct(cb->blockBio);
#else
    dm_submit_bio_remap(cb->blockBio, NULL);
#endif
  }

  // Grab the cache block lock, as we are expected to hold it when we return.
  spin_lock_irq(&cb->lock);
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio that can be serviced
 * using a cache block.
 *
 * @param cb     The cache block (locked)
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 **/
static void processBioCached(CacheBlock      *cb,
                             struct bio      *bio,
                             struct bio_list *ready)
{
  DoryDevice *dd = cb->doryDevice;

  // Set the block state to COPYING, and release the cache block lock.  We do
  // not want to hold the lock while we copy the data.
  cb->state = COPYING;
  spin_unlock_irq(&cb->lock);

  // Compute the cache address to begin transfers.
  sector_t blockNumber = getBioSector(bio) >> dd->blockShift;
  size_t offset = (getBioSector(bio) - (blockNumber << dd->blockShift)) << 9;
  char *data = cb->blockData + offset;

  // Copy the data.
  struct bio_vec bv;
  for (struct bvec_iter iter = bio->bi_iter; (iter.bi_size > 0);
       bio_advance_iter(bio, &iter, bv.bv_len)) {
    bv = bio_iter_iovec(bio, iter);
    char *buffer = page_address(bv.bv_page) + bv.bv_offset;
    if (bio_data_dir(bio) == READ) {
      memcpy(buffer, data, bv.bv_len);
    } else {
      memcpy(data, buffer, bv.bv_len);
    }
    data += bv.bv_len;
  }

  // We are done with the bio.
  endio(bio, 0);
  atomic64_inc(&dd->successBios);

  // Grab the cache block lock, and set the block state to DIRTY.
  spin_lock_irq(&cb->lock);
  cb->state = DIRTY;

  // We can immediately release the waiting bios.
  bio_list_merge(ready, &cb->waitingBios);
  bio_list_init(&cb->waitingBios);

  // See whether a flush request has asked to flush all blocks.  Note that this
  // check is made without holding the flush lock.  This is safe because
  // flushFlag is true only because it was set after our bio began processing
  // and the flushTheCache() missed this cache block while we were in COPYING
  // state.  The cache block spinlock has provided us with adequate memory
  // barriers.
  if (dd->flushFlag) {
    flushCacheBlock(cb);
  }
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio that is possibly in the
 * cache.
 *
 * @param cb     The cache block (locked)
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 *
 * @return one of these specific values:
 *
 *         DM_MAPIO_REMAPPED to indicate the the bio is ready for submit_bio.
 *
 *         DM_MAPIO_SUBMITTED to indicate that the bio will be processed by
 *                            dmDory, either by processing it completely and
 *                            calling bio_endio, or forwarding it onward by
 *                            submitting it to the next layer.
 **/
static int processBioLocked(CacheBlock      *cb,
                            struct bio      *bio,
                            struct bio_list *ready)
{
  DoryDevice *dd = cb->doryDevice;
  sector_t blockNumber = getBioSector(bio) >> dd->blockShift;
  if (cb->state == EMPTY) {
    // Cache block is unused.  Look for a reason to do the I/O directly.  In
    // order: It's a read; it's a REQ_FUA; it's a REQ_DISCARD; it's a partial
    // block.
    if ((bio_data_dir(bio) == READ)
        || isFUABio(bio)
        || isDiscardBio(bio)
        || (getBioSize(bio) < dd->blockSize)) {
      return DM_MAPIO_REMAPPED;
    }
    // We have an unused cache block for an ordinary write of a full block.
    // But filter out some blocks.  The default mask/modulus settings will
    // cause the block to be cached.  We expect to use these defaults for 4K
    // blocks.  When the blocksize if 512, we expect that the mask/modules
    // settings will be used to test with torn writes.
    if ((dd->tornMask & (1 << (blockNumber % dd->tornModulus))) == 0) {
      return DM_MAPIO_REMAPPED;
    }
    // Use this cache block.  This is an EMPTY to DIRTY transition, so bump the
    // busy count.
    atomic_inc(&dd->busyCount);
    cb->blockNumber = blockNumber;
    processBioCached(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (cb->blockNumber != blockNumber) {
    // This is not the block we are looking for.
    return DM_MAPIO_REMAPPED;
  };

  // We found this block in the cache.
  if (cb->state != DIRTY) {
    // The block is busy, so we must wait.
    bio_list_add(&cb->waitingBios, bio);
    return DM_MAPIO_SUBMITTED;
  } else if (!isFUABio(bio) && !isDiscardBio(bio)) {
    // Unless it is a FUA write or a discard, we can service the bio directly
    // using the cache.
    processBioCached(cb, bio, ready);
    return DM_MAPIO_SUBMITTED;
  } else if (getBioSize(bio) == dd->blockSize) {
    // It's a full block FUA write or discard, so drop the cache block and just
    // do the write.  Because our bio is known to be busy, this can never drop
    // the busy count to zero.
    cb->state = EMPTY;
    atomic_dec(&dd->busyCount);
    return DM_MAPIO_REMAPPED;
  } else {
    // It's a partial block FUA write or discard, so wait while we flush the
    // whole cached block to storage.
    bio_list_add(&cb->waitingBios, bio);
    flushCacheBlock(cb);
    return DM_MAPIO_SUBMITTED;
  }
}

/**********************************************************************/
/**
 * Flush all of the cached data to the storage medium.
 *
 * @param dd  The Dory device
 **/
static void flushTheCache(DoryDevice *dd)
{
  for (unsigned int i = 0; i < dd->cacheBlockCount; i++) {
    CacheBlock *cb = &dd->cacheBlocks[i];
    spin_lock_irq(&cb->lock);
    if (cb->state == DIRTY) {
      flushCacheBlock(cb);
    }
    spin_unlock_irq(&cb->lock);
  }
}

/**********************************************************************/
/**
 * Process an I/O request encapsulated in a struct bio.
 *
 * @param dd     The Dory device
 * @param bio    The I/O request to be processed
 * @param ready  bio list of bios that are now unblocked and can be processed
 *               after we return
 *
 * @return one of these specific values:
 *
 *         DM_MAPIO_REMAPPED to indicate that the bio is ready for submit_bio.
 *
 *         DM_MAPIO_SUBMITTED to indicate that the bio will be processed by
 *                            dmDory, either by processing it completely and
 *                            calling bio_endio, or forwarding it onward by
 *                            submitting it to the next layer.
 **/
static int processBio(DoryDevice *dd, struct bio *bio, struct bio_list *ready)
{
  if ((bio_data_dir(bio) == WRITE) && dd->stopFlag) {
    // We have been told to stop writing.  Make it so.
    atomic64_inc(&dd->writeFailure);
    endio(bio, dd->ioError);
    return DM_MAPIO_SUBMITTED;
  }

  if (dd->cacheBlockCount == 0) {
    // We are not doing caching.  Just go ahead and do the I/O.
    return DM_MAPIO_REMAPPED;
  }

  // We are doing caching.  When this busy count returns to zero, it will be
  // time to acknowledge empty flushes.
  atomic_inc(&dd->busyCount);

  spin_lock_irq(&dd->flushLock);
  int result;
  if (isFlushBio(bio)) {
    if (getBioSize(bio) > 0) {
      printk(KERN_WARNING "flush bio too big!");
    }
    // Add to the list of active flush bios.  If we are the first one, we must
    // initiate flushing the cache.
    bio_list_add(&dd->flushBios, bio);
    bool firstFlush = !dd->flushFlag;
    dd->flushFlag = true;
    spin_unlock_irq(&dd->flushLock);
    if (firstFlush) {
      flushTheCache(dd);
    }
    result = DM_MAPIO_SUBMITTED;
  } else if (dd->flushFlag) {
    // A flush is in progress.  Need to defer this bio.
    bio_list_add(&dd->waitingBios, bio);
    spin_unlock_irq(&dd->flushLock);
    result = DM_MAPIO_SUBMITTED;
  } else {
    spin_unlock_irq(&dd->flushLock);
    // There is no flush in progress, so we may lock the cache block and
    // proceed to do the I/O.
    sector_t blockNumber = getBioSector(bio) >> dd->blockShift;
    unsigned int slotNumber = blockNumber % dd->cacheBlockCount;
    CacheBlock *cb = &dd->cacheBlocks[slotNumber];
    spin_lock_irq(&cb->lock);
    result = processBioLocked(cb, bio, ready);
    spin_unlock_irq(&cb->lock);
  }

  // We have finished working on this bio.
  decrementBusyCountAndTest(dd);
  return result;
}

/**********************************************************************/
/**
 * Process a list of delayed I/O requests encapsulated in a struct bio_list.
 *
 * @param dd     The Dory device
 * @param ready  bio list of bios that are ready to process
 **/
static void processBioList(DoryDevice *dd, struct bio_list *ready)
{
  struct bio *bio;
  while ((bio = bio_list_pop(ready)) != NULL) {
    if (processBio(dd, bio, ready) == DM_MAPIO_REMAPPED) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)
      submit_bio_noacct(bio);
#else
      dm_submit_bio_remap(bio, NULL);
#endif
      atomic64_inc(&dd->submittedBios);
    }
  }
}

/**********************************************************************/
static void freeDoryDeviceCache(DoryDevice *dd)
{
  // Free the cache data blocks.
  if (dd->cacheData != NULL) {
    vfree(dd->cacheData);
  }

  // Free the bios for the cache data blocks.
  for (unsigned int i = 0; i < dd->cacheBlockCount; i++) {
    CacheBlock *cb = &dd->cacheBlocks[i];
    if (cb->blockBio != NULL) {
      bio_uninit(cb->blockBio);
      kfree(cb->blockBio);
    }
  }
}

/**********************************************************************/
// BEGIN Dory device methods for the dory target type
/**********************************************************************/
static int doryCtr(struct dm_target *ti, unsigned int argc, char **argv)
{
  if (argc != 4) {
    ti->error = "requires exactly 4 arguments";
    return -EINVAL;
  }
  const char *doryName   = argv[0];
  const char *devicePath = argv[1];
  unsigned long long blockSize, cacheBlockCount;
  char dummy;
  if ((sscanf(argv[2], "%llu%c", &blockSize, &dummy) != 1)
      || ((blockSize != 512) && (blockSize != 4096))) {
    ti->error = "Invalid block size";
    return -EINVAL;
  }
  if ((sscanf(argv[3], "%llu%c", &cacheBlockCount, &dummy) != 1)
      || (cacheBlockCount > 0xFFEC)) {
    ti->error = "Invalid cache size";
    return -EINVAL;
  }

  DoryDevice *dd = kzalloc(sizeof(DoryDevice)
                           + cacheBlockCount * sizeof(CacheBlock),
                           GFP_KERNEL);
  if (dd == NULL) {
    ti->error = "Cannot allocate context";
    return -ENOMEM;
  }
  char *cacheData = NULL;
  if (cacheBlockCount > 0) {
    cacheData = __vmalloc(cacheBlockCount * blockSize, GFP_KERNEL);
    if (cacheData == NULL) {
      kfree(dd);
      ti->error = "Cannot allocate cache";
      return -ENOMEM;
    }
  }
  dd->blockShift      = blockSize == 4096 ? 3 : 0;
  dd->blockSize       = blockSize;
  dd->cacheData       = cacheData;
  dd->cacheBlockCount = cacheBlockCount;
  dd->ioError         = BIO_EIO;
  dd->stopFlag        = false;
  dd->tornMask        = ~0;
  dd->tornModulus     = 8;
  strncpy(dd->doryName, doryName, DORY_NAME_SIZE);
  bio_list_init(&dd->flushBios);
  bio_list_init(&dd->waitingBios);
  bio_list_init(&dd->workBios);
  bio_list_init(&dd->workFlushBios);
  spin_lock_init(&dd->flushLock);
  spin_lock_init(&dd->workLock);
  for (unsigned int i = 0; i < cacheBlockCount; i++) {
    CacheBlock *cb = &dd->cacheBlocks[i];
    bio_list_init(&cb->waitingBios);
    spin_lock_init(&cb->lock);
    cb->blockBio = bio_kmalloc(1, GFP_KERNEL);
    cb->blockData = cacheData;
    cb->doryDevice = dd;
    cb->state = EMPTY;
    cacheData += blockSize;
    if (cb->blockBio == NULL) {
      freeDoryDeviceCache(dd);
      kfree(dd);
      ti->error = "Cannot allocate cache bio";
      return -ENOMEM;
    }
  }

  if (dmGetDevice(ti, devicePath, &dd->dev)) {
    ti->error = "Device lookup failed";
    freeDoryDeviceCache(dd);
    kfree(dd);
    return -EINVAL;
  }

  kobject_init(&dd->kobj, &doryObjectType);
  int result = kobject_add(&dd->kobj, &doryKobj, dd->doryName);
  if (result < 0) {
    ti->error = "sysfs addition failed";
    dm_put_device(ti, dd->dev);
    freeDoryDeviceCache(dd);
    kfree(dd);
    return result;
  }

  ti->flush_supported = 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0)
  ti->accounts_remapped_io = 1;
#endif
  BUG_ON(dm_set_target_max_io_len(ti, blockSize >> 9) != 0); 
  ti->num_flush_bios = 1;
  ti->private = dd;
  return 0;
}

/**********************************************************************/
static void doryDtr(struct dm_target *ti)
{
  DoryDevice *dd = ti->private;
  dm_put_device(ti, dd->dev);
  freeDoryDeviceCache(dd);
  kobject_put(&dd->kobj);
}

/**********************************************************************/
static int doryMap(struct dm_target *ti,
                   struct bio       *bio)
{
  DoryDevice *dd = ti->private;

  // Map the I/O to the storage device.
  setBioBlockDevice(bio, dd->dev->bdev);
  setBioSector(bio, dm_target_offset(ti, getBioSector(bio)));

  // Perform accounting.
  if (bio_data_dir(bio) == READ) {
    atomic64_inc(&dd->readTotal);
  } else {
    if (isFlushBio(bio)) {
      atomic64_inc(&dd->flushTotal);
      dd->readsAtLastFlush  = atomic64_read(&dd->readTotal);
      dd->writesAtLastFlush = atomic64_read(&dd->writeTotal);
    }
    if (isFUABio(bio)) {
      atomic64_inc(&dd->fuaTotal);
    }
    if (getBioSize(bio) > 0) {
      atomic64_inc(&dd->writeTotal);
    }
  }

  // Process the already mapped I/O.
  struct bio_list readyList;
  bio_list_init(&readyList);
  int result = processBio(dd, bio, &readyList);

  // If the processing released any other bio requests, process them now.  This
  // indirect method of making a list to process one at a time ensures that we
  // do not overrun the small kernel stack.
  processBioList(dd, &readyList);

  // Perform return value accounting.
  if (result == DM_MAPIO_REMAPPED) {
    atomic64_inc(&dd->mappedReturns);
  } else if (result == DM_MAPIO_SUBMITTED) {
    atomic64_inc(&dd->submittedReturns);
  }
  return result;
}

/**********************************************************************/
static void doryStatus(struct dm_target *ti,
                       status_type_t     type,
                       unsigned int      status_flags,
                       char             *result,
                       unsigned int      maxlen)
{
  DoryDevice *dd = ti->private;
  unsigned int sz = 0;  // used by the DMEMIT macro

  switch (type) {
  case STATUSTYPE_INFO:
    result[0] = '\0';
    break;

  case STATUSTYPE_TABLE:
    DMEMIT("%s %s %zu %u", dd->doryName, dd->dev->name, dd->blockSize,
           dd->cacheBlockCount);
    break;
  case STATUSTYPE_IMA:
    *result = '\0';
    break;
  }
}

/**********************************************************************/
static struct target_type doryTargetType = {
  .name            = "dory",
  .version         = { 1, 0, 0 },
  .module          = THIS_MODULE,
  .ctr             = doryCtr,
  .dtr             = doryDtr,
  .iterate_devices = commonIterateDevices,
  .map             = doryMap,
  .status          = doryStatus,
  // Put version specific functions at the bottom
  .prepare_ioctl   = commonPrepareIoctl,
};

/**********************************************************************/
int __init doryInit(void)
{
  BUILD_BUG_ON(offsetof(DoryDevice, dev) != offsetof(CommonDevice, dev));

  kobject_init(&doryKobj, &emptyObjectType);
  int result = kobject_add(&doryKobj, NULL, THIS_MODULE->name);
  if (result < 0) {
    return result;
  }

  result = dm_register_target(&doryTargetType);
  if (result < 0) {
    kobject_put(&doryKobj);
    DMERR("dm_register_target failed %d", result);
  }
  return result;
}

/**********************************************************************/
void __exit doryExit(void)
{
  dm_unregister_target(&doryTargetType);
  kobject_put(&doryKobj);
}

module_init(doryInit);
module_exit(doryExit);

MODULE_DESCRIPTION(DM_NAME " dory testing device");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
