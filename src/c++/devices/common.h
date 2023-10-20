/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 *
 * Common code for the testfua module, which contains device mapper devices
 * used by VDO test code.  This file provides:
 *
 *  - Common definitions that encapsulate Linux version differences.
 *  - Prototypes for code common to different test devices.
 *  - External declarations for module data.
 */

#ifndef COMMON_H
#define COMMON_H

#include <linux/blk_types.h>
#include <linux/build_bug.h>
#include <linux/device-mapper.h>
#include <linux/kobject.h>
#include <linux/version.h>
#include <uapi/linux/dm-ioctl.h>

/**
 * Define SECTOR_SIZE if it currently isn't.
 **/
#if !defined(SECTOR_SIZE)
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#endif // !defined(SECTOR_SIZE)

typedef blk_status_t BioStatusType;
enum { BIO_SUCCESS = BLK_STS_OK,
       BIO_EIO     = BLK_STS_IOERR};

typedef struct commonDevice {
  // Pointer to the underlying storage device. MUST BE FIRST ITEM IN STRUCT.
  struct dm_dev        *dev;
} CommonDevice;

/**********************************************************************/
static inline bool isDiscardBio(struct bio *bio)
{
  return (bio_op(bio) == REQ_OP_DISCARD);
}

/**********************************************************************/
static inline bool isFlushBio(struct bio *bio)
{
  return (bio_op(bio) == REQ_OP_FLUSH) || ((bio->bi_opf & REQ_PREFLUSH) != 0);
}

/**********************************************************************/
static inline bool isFUABio(struct bio *bio)
{
  return (bio->bi_opf & REQ_FUA) != 0;
}

/**********************************************************************/
static inline bool isReadBio(struct bio *bio)
{
  return bio_data_dir(bio) == READ;
}

/**********************************************************************/
static inline bool isWriteBio(struct bio *bio)
{
  return bio_data_dir(bio) == WRITE;
}

/**
 * Get a bio's size.
 *
 * @param bio  The bio
 *
 * @return the bio's size
 **/
static inline unsigned int getBioSize(struct bio *bio)
{
  return bio->bi_iter.bi_size;
}

/**
 * Set the bio's sector.
 *
 * @param bio     The bio
 * @param sector  The sector
 **/
static inline void setBioSector(struct bio *bio, sector_t sector)
{
  bio->bi_iter.bi_sector = sector;
}

/**
 * Get the bio's sector.
 *
 * @param bio  The bio
 *
 * @return the sector
 **/
static inline sector_t getBioSector(struct bio *bio)
{
  return bio->bi_iter.bi_sector;
}

/**
 * Set the block device for a bio.
 *
 * @param bio     The bio to modify
 * @param device  The new block device for the bio
 **/
static inline void setBioBlockDevice(struct bio          *bio,
                                     struct block_device *device)
{
  bio_set_dev(bio, device);
}

/**
 * Get the error from the bio.
 *
 * @param bio  The bio
 *
 * @return the bio's error if any
 **/
static inline int getBioResult(struct bio *bio)
{
  return blk_status_to_errno(bio->bi_status);
}

/**
 * Clone a bio.
 *
 * @param  bio  The bio to clone
 * @param  bs   The bio set to use for cloning
 *
 * @return the cloned bio, or NULL if none could be allocated
 **/
static inline struct bio *cloneBio(struct bio *bio,
                                   struct bio_set *bs)
{
#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0))
#endif

#if USE_ALTERNATE
  return bio_clone_fast(bio, GFP_KERNEL, bs);
#else
  return bio_alloc_clone(bio->bi_bdev, bio, GFP_KERNEL, bs);
#endif
}

/**
 * Invoke the bi_end_io callback routine
 *
 * @param bio    The bio
 * @param error  An error indication
 **/
static inline void endio(struct bio *bio, BioStatusType error)
{
  bio->bi_status = error;
  bio_endio(bio);
}

/**********************************************************************/

// sysfs type for an "empty" directory (other directories can be added to it)
extern struct kobj_type emptyObjectType;

/**********************************************************************/
char *bufferToString(const char *buf, size_t length);

/**********************************************************************/
int commonPrepareIoctl(struct dm_target *ti, struct block_device **bdev);

/**********************************************************************/
int commonIterateDevices(struct dm_target           *ti,
                         iterate_devices_callout_fn  fn,
                         void                       *data);

/**********************************************************************/
int dmGetDevice(struct dm_target  *ti,
                const char        *path,
                struct dm_dev    **devPtr);

/**********************************************************************
 * Checks whether the argument passed in is the string we want to
 * compare against.
 *
 * @param [in] arg        the argument to check
 * @param [in] thisOption the value to compare against
 *
 * @return true if argument matches value, false otherwise
 */
static inline bool isArgString(const char *arg, const char *thisOption)
{
  // device-mapper convention seems to be case-independent options
  return strncasecmp(arg, thisOption, strlen(thisOption)) == 0;
}

/**
 * Returns a boolean indicating if the specified dmsetup message is a global
 * disable (don't perform the device's processing; just pass through) message.
 *
 * @param [in]  argc    message argument count
 * @param [in]  argv    message argument vector
 *
 * @return  boolean   true => message to disable all device processing and
 *                            just pass through requests
 **/
static inline int isGlobalDisableMessage(unsigned int argc, char * const *argv)
{
  return (argc == 1) && isArgString(argv[0], "disable");
}

/**
 * Returns a boolean indicating if the specified dmsetup message is a global
 * enable (perform the device's operations) message.
 *
 * @param [in]  argc    message argument count
 * @param [in]  argv    message argument vector
 *
 * @return  boolean   true => message to enable device processing
 **/
static inline int isGlobalEnableMessage(unsigned int argc, char * const *argv)
{
  return (argc == 1) && isArgString(argv[0], "enable");
}

#endif /* COMMON_H */
