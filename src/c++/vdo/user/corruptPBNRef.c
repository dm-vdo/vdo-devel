/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"
#include "syscalls.h"

#include "packed-reference-block.h"
#include "status-codes.h"
#include "types.h"

#include "blockMapUtils.h"
#include "fileLayer.h"
#include "slabSummaryReader.h"
#include "userVDO.h"
#include "vdoVolumeUtils.h"

static char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];

static const char usageString[] = "[--help] filename";
static const char helpString[]  =
  "corruptPBNRef - alter the reference count of a mapped physical block\n"
  "\n"
  "SYNOPSIS\n"
  "  corruptPBNRef <path> | help \n"
  "\n"
  "DESCRIPTION\n"
  "  corruptPBNRef will alter the reference count of the first pbn mapped\n"
  "  to by the lowest numbered lbn which is mapped to something other than\n"
  "  the zero block.\n"
  "\n"
  "  The <path> argument should specify the VDO backing file to corrupt.\n"
  "\n";

static struct option options[] = {
  { "help", no_argument, NULL, 'h' },
  { NULL,   0,           NULL,  0  },
};

static void usage(const char *progname, const char *usageOptionsString)
{
  fprintf(stderr, "Usage: %s %s\n", progname, usageOptionsString);
  exit(1);
}

/**
 * Get the VDO filename or print the help string and exit.
 *
 * @param [in]  argc      Number of input arguments
 * @param [in]  argv      The array of input arguments
 * @param [out] filename  Where the VDO is
 **/
static void processCorruptorArguments(int    argc,
                                      char  *argv[],
                                      char **filename)
{
  int   c;
  char *optionString = "h";
  while ((c = getopt_long(argc, argv, optionString, options, NULL)) != -1) {
    if (c == (int) 'h') {
      printf("%s", helpString);
      exit(0);
    }
  }

  if (optind != (argc - 1)) {
    usage(argv[0], usageString);
  }

  *filename = argv[optind];
}

/**
 * Get the slab ID from the PBN entry to corrupt.
 *
 * @param [in]   vdo         The VDO to corrupt
 * @param [in]   targetPBN   The PBN index to corrupt
 * @param [out]  slabID      The slab which contains the targetPBN
 *
 * @return VDO_SUCCESS or error code
 **/
static int findTargetSlabID(UserVDO                 *vdo,
                            physical_block_number_t  targetPBN,
                            slab_count_t            *slabID)
{
  slab_count_t slabNumber = ((targetPBN - vdo->states.slab_depot.first_block)
                             / vdo->states.vdo.config.slab_size);
  if (slabNumber >= vdo->slabCount) {
    warnx("Target slab %u must be less than VDO slab count %u",
          slabNumber, vdo->slabCount);
    return VDO_OUT_OF_RANGE;
  }

  warnx("Target slab will be ID# %u", slabNumber);
  *slabID = slabNumber;
  return VDO_SUCCESS;
}

/**
 * Calculate the origin and load the refCountBuffer.
 *
 * @param [in]   vdo              The VDO to corrupt
 * @param [in]   firstBlockOffset PBN of the first block in the slab
 * @param [in]   slabBlockNumber  Offset of the target in the slab
 * @param [out]  targetRefCount   PBN of the target reference count block
 *
 * @return VDO_SUCCESS or an error
 **/
static int getSlabRefCountBlock(UserVDO                 *vdo,
                                physical_block_number_t  firstBlockOffset,
                                physical_block_number_t  slabBlockNumber,
                                physical_block_number_t *targetRefCount)
{
  // Get the refCounts stored on this used slab.
  physical_block_number_t refCountOrigin
    = firstBlockOffset + vdo->states.slab_depot.slab_config.data_blocks;

  *targetRefCount = refCountOrigin + (slabBlockNumber / COUNTS_PER_BLOCK);
  return VDO_SUCCESS;
}

/**
 * Corrupt a VDO by changing the reference count for the first mapped LBN.
 *
 * @param vdo  The VDO to corrupt
 *
 * @return VDO_SUCCESS or an error
 **/
static int corrupt(UserVDO *vdo)
{
  // Load the slab summary for this VDO, to ensure the refcount
  // change in this slab will have an effect.
  struct slab_summary_entry *summaryEntries = NULL;
  int result = readSlabSummary(vdo, &summaryEntries);
  if (result != VDO_SUCCESS) {
    // Could not load the slab summary.
    warnx("Failed to load the slab summary: %s.",
          uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
    return result;
  }

  for (logical_block_number_t lbn = 0;
       lbn < vdo->states.vdo.config.logical_blocks;
       lbn++) {
    physical_block_number_t  pbn;
    enum block_mapping_state state;
    result = findLBNMapping(vdo, lbn, &pbn, &state);
    if (result != VDO_SUCCESS) {
      warnx("Error retrieving mapping for LBN %llu: %s",
            (unsigned long long) lbn,
	    uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
      break;
    }

    if (state == VDO_MAPPING_STATE_UNMAPPED) {
      continue;
    }

    slab_count_t slabNumber;
    result = findTargetSlabID(vdo, pbn, &slabNumber);
    if (result != VDO_SUCCESS) {
      break;
    }

    if (!summaryEntries[slabNumber].load_ref_counts) {
      // The slab has never finished saving its refcounts, so changing them
      // would have no effect.
      continue;
    }

    physical_block_number_t firstBlockOffset
      = (vdo->states.slab_depot.first_block
         + (slabNumber * vdo->states.vdo.config.slab_size));
    physical_block_number_t slabBlockNumber = pbn - firstBlockOffset;

    physical_block_number_t targetRefCount;
    result = getSlabRefCountBlock(vdo, firstBlockOffset, slabBlockNumber,
                                  &targetRefCount);
    if (result != VDO_SUCCESS) {
      break;
    }

    PhysicalLayer *layer = vdo->layer;
    char *buffer;
    result = layer->allocateIOBuffer(layer, VDO_BLOCK_SIZE, "reference block",
                                     &buffer);
    if (result != VDO_SUCCESS) {
      warnx("Could not allocate reference count block");
      break;
    }

    result = layer->reader(layer, targetRefCount, 1, buffer);
    if (result != VDO_SUCCESS) {
      warnx("Could not read reference count for target %llu",
	    (unsigned long long) pbn);
      break;
    }

    warnx("LBN %llu maps to PBN %llu",
	  (unsigned long long) lbn, (unsigned long long) pbn);

    // Change the refCount in the appropriate entry.
    struct packed_reference_block  *block
      = (struct packed_reference_block *) buffer;
    physical_block_number_t    blockIndex
      = slabBlockNumber % COUNTS_PER_BLOCK;
    sector_count_t             sectorNumber    = blockIndex / COUNTS_PER_SECTOR;
    physical_block_number_t    sectorIndex     = blockIndex % COUNTS_PER_SECTOR;
    struct packed_reference_sector *sector = &block->sectors[sectorNumber];
    warnx("ref count was %u\n", sector->counts[sectorIndex]);
    sector->counts[sectorIndex]         = 255 - sector->counts[sectorIndex];
    warnx("ref count is %u\n", sector->counts[sectorIndex]);
    // Now write it out to corrupt the entry.
    result = layer->writer(layer, targetRefCount, 1, buffer);
    if (result != VDO_SUCCESS) {
      warnx("Could not write reference count buffer for slab number %u\n",
            slabNumber);
    }

    UDS_FREE(buffer);
    break;
  }

  UDS_FREE(summaryEntries);
  return result;
}

/**********************************************************************/
int main(int argc, char *argv[])
{
  static char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];

  int result = vdo_register_status_codes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register status codes: %s",
         uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
  }

  char *filename;
  processCorruptorArguments(argc, argv, &filename);

  UserVDO *vdo = NULL;
  result = makeVDOFromFile(filename, false, &vdo);
  if (result != VDO_SUCCESS) {
    errx(1, "failed to create layer or VDO from %s : %s", filename,
         uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
  }

  result = corrupt(vdo);
  freeVDOFromFile(&vdo);
  exit((result == VDO_SUCCESS) ? 0 : 1);
}
