/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "status-codes.h"
#include "types.h"
#include "vdoConfig.h"

#include "blockMapUtils.h"
#include "parseUtils.h"
#include "printUtils.h"

/* Small index size might be wrong */
#define CHAPTER_PER_GIG             1024
#define DEFAULT_BLOCKMAP_CACHE_SIZE 32768
#define DEFAULT_GEOMETRY_BLOCK      1
#define DEFAULT_SUPERBLOCK          1

#define PPRETTY vdoInfo->humanReadable

/* Should change MIN_SLAB_BITS */
enum {
  MIN_SLAB_BITS        = 13,
  DEFAULT_SLAB_BITS    = 19,
};

#define FIXED_METADATA_BLOCKS DEFAULT_SUPERBLOCK + \
                              DEFAULT_GEOMETRY_BLOCK + \
                              DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT + \
                              VDO_SLAB_SUMMARY_BLOCKS + \
                              DEFAULT_VDO_RECOVERY_JOURNAL_SIZE

struct vdoInfo{
  /* VDO calculate parament */
  char *indexMemorySize;
  u64 logicalSize;
  u64 physicalSize;
  int sparseIndex;
  unsigned int slabBits;

  /* UDS info */
  u64 udsIndexSize;
  u64 dedupeWindowSize;

  /* VDO block info */
  u64 physicalBlocks;
  u64 logicalBlocks;
  u64 userDataBlocks;
  int totalSystemBlock;

  /* Slab info */
  int slabBlockCount;
  u64 slabCount;
  u64 totalSlabJouranl;
  u64 totalReferenceCount;

  /* Blockmap info */
  u64 blockMapCacheSize;
  u64 totalBlockMapPages;
  u64 totalBlockMapLeaves;
  u64 totalUsableSpace;
  u64 totalForestMemoryUsage;

  int humanReadable;
};

/*
  A VDO volume contains three types of data, system metadata, user metadata
  and user data. All VDO data is stored in a 4K block.

  System metadata are constant regardless the size of VDO volume.  They are:
  Super Block 1
  Geometry Block 1
  Block map root 60
  Slab Summary 64
  Recovery Journal 32K
  UDS index - Created during VDO format.

  The rest of the data are divided into slabs. Within each slab, it contains
  user data and three types of metadata, block map page, reference count
  and slab journal. These meta data usages depend on the size of the VDO and
  the size of each slab.

  Slab size is configurable during VDO format. Default slab size is 2G.
  Slab size can be adjusted using slab bit, default is 19. That is 2 to
  the power of 19 and it is 2G.

  Usable space is calculated by subtracting all the meta data usage from VDO a
  volume.
*/

/*
  Each block map page contains 812 entries, each logical block is one entry.
  First, calculate the total leaves pages by dividing 812. We then calculate
  additional block map pages at the upper level.
*/
static u64 getBlockMapPages(u64 logicalBlocks)
{
  u64 totalBlockMapLeaves = vdo_compute_block_map_page_count(logicalBlocks);
  u64 parentBlockUsage = totalBlockMapLeaves;
  int additionalPage = 0;
  for(int i=0; i < VDO_BLOCK_MAP_TREE_HEIGHT - 1; i++) {
    parentBlockUsage = DIV_ROUND_UP(parentBlockUsage,
				    VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
    additionalPage += parentBlockUsage;
  }
  return totalBlockMapLeaves + additionalPage;
}

static u64 getSlabBlockCount(unsigned int slabBits)
{
  u64 blockCount = 1U << slabBits;
  block_count_t numberOfBlocks = (block_count_t) blockCount;
  return numberOfBlocks;
}

static u64 getUDSIndexSize(char *memorySize, int sparseIndex)
{
  int result;
  UdsConfigStrings configStrings;
  char errorBuffer[VDO_MAX_ERROR_MESSAGE_SIZE];

  memset(&configStrings, 0, sizeof(configStrings));
  if (sparseIndex)
    configStrings.sparse = "1";
  configStrings.memorySize = memorySize;

  struct index_config indexConfig;
  result = parseIndexConfig(&configStrings, &indexConfig);
  if (result != VDO_SUCCESS) {
    errx(result, "parseIndexConfig failed: %s",
         uds_string_error(result, errorBuffer, sizeof(errorBuffer)));
  }

  block_count_t indexBlocks = 0;
  result = computeIndexBlocks(&indexConfig, &indexBlocks);
  if (result != VDO_SUCCESS) {
    errx(result, "computeIndexBlocks failed: %s",
         uds_string_error(result, errorBuffer, sizeof(errorBuffer)));
  }
  return indexBlocks;
}

/*
  For deduplication window calculation, please refer to geometry.c in
  utils/uds/geometry.c for detail on how deduplication window is calculated.
 */
static u64 getDedupeWindowSize(char *indexMemorySize)
{
  if (strcmp(indexMemorySize, "0.25") == 0) {
    return 256;
  } else if ((strcmp(indexMemorySize, "0.5") == 0)
             || (strcmp(indexMemorySize, "0.50") == 0)) {
    return 512;
  } else if (strcmp(indexMemorySize, "0.75") == 0) {
    return 768;
  } else {
    int number;
    int result = parseInt(indexMemorySize, &number);
    if (result != VDO_SUCCESS) {
      errx(result, "parseInt failed: getDedupeWindowSize");
    }
    return number * CHAPTER_PER_GIG;
  }
}

static void calculateVDOBlockInfo(struct vdoInfo *vdoInfo)
{
  vdoInfo->totalSystemBlock = FIXED_METADATA_BLOCKS + vdoInfo->udsIndexSize;
  vdoInfo->physicalBlocks = vdoInfo->physicalSize / VDO_BLOCK_SIZE;
  vdoInfo->logicalBlocks = vdoInfo->logicalSize / VDO_BLOCK_SIZE;
  vdoInfo->userDataBlocks = vdoInfo->physicalBlocks - vdoInfo->totalSystemBlock;
}

static void calculateSlabInfo(struct vdoInfo *vdoInfo)
{
  vdoInfo->slabBlockCount = getSlabBlockCount(vdoInfo->slabBits);
  vdoInfo->slabCount = vdoInfo->userDataBlocks / vdoInfo->slabBlockCount;
  vdoInfo->totalSlabJouranl = vdoInfo->slabCount * DEFAULT_VDO_SLAB_JOURNAL_SIZE;
  vdoInfo->totalReferenceCount =
    vdo_get_saved_reference_count_size(vdoInfo->userDataBlocks);
}

static void calculateBlockMapMetaInfo(struct vdoInfo *vdoInfo)
{
  vdoInfo->totalBlockMapLeaves =
    vdo_compute_block_map_page_count(vdoInfo->logicalBlocks);
  vdoInfo->totalBlockMapPages = getBlockMapPages(vdoInfo->logicalBlocks);
  vdoInfo->totalForestMemoryUsage = vdoInfo->totalBlockMapPages -
                                    vdoInfo->totalBlockMapLeaves;
  vdoInfo->totalUsableSpace = (vdoInfo->userDataBlocks -
			       vdoInfo->totalBlockMapPages -
			       vdoInfo->totalReferenceCount -
			       vdoInfo->totalSlabJouranl) *
                              VDO_BLOCK_SIZE;
}

static void calculateVDOInfo(struct vdoInfo *vdoInfo)
{
  vdoInfo->dedupeWindowSize = getDedupeWindowSize(vdoInfo->indexMemorySize);
  if (vdoInfo->sparseIndex) {
    vdoInfo->dedupeWindowSize *= 10;
  }
  vdoInfo->udsIndexSize = getUDSIndexSize(vdoInfo->indexMemorySize,
					  vdoInfo->sparseIndex);
  calculateVDOBlockInfo(vdoInfo);
  calculateSlabInfo(vdoInfo);
  calculateBlockMapMetaInfo(vdoInfo);
}

static void printVDOInputParameters(struct vdoInfo *vdoInfo)
{
  printf("Input Parameters:\n");
  printf("  Physical Size: %s\n",
	 getSizeString(vdoInfo->physicalBlocks * VDO_BLOCK_SIZE,
		       PPRETTY));
  printf("  Logical Size: %s\n",
	 getSizeString(vdoInfo->logicalBlocks * VDO_BLOCK_SIZE,
		       PPRETTY));
  printf("  Slab Bits: %d\n", vdoInfo->slabBits);
  printf("  Sparse: %d\n", vdoInfo->sparseIndex);
  printf("  Index Memory: %s\n", vdoInfo->indexMemorySize);
}

static void printVDOStorageUsage(struct vdoInfo *vdoInfo)
{
  printf("Storage Usage:\n");
  printf("  Total BlockMap Pages: %s\n",
	 getSizeString((vdoInfo->totalBlockMapPages * VDO_BLOCK_SIZE),
		       PPRETTY));
  printf("  UDS Index Size: %s\n",
	 getSizeString((vdoInfo->udsIndexSize * VDO_BLOCK_SIZE), PPRETTY));
  printf("  Slab Reference Count Usage: %s\n",
         getSizeString(
	   (vdo_get_saved_reference_count_size(vdoInfo->userDataBlocks) *
            VDO_BLOCK_SIZE), PPRETTY));
  printf("  Slab Jouranl Usage: %s\n",
         getSizeString((vdoInfo->slabCount * DEFAULT_VDO_SLAB_JOURNAL_SIZE *
			VDO_BLOCK_SIZE), PPRETTY));
}

static void printVDOMemoryUsage(struct vdoInfo *vdoInfo)
{
  printf("In Memory Usage:\n");
  printf("  Block Map Cache: %s\n",
         getSizeString((vdoInfo->blockMapCacheSize * VDO_BLOCK_SIZE),
		       PPRETTY));
  printf("  Forest Memory Usage: %s\n",
         getSizeString((vdoInfo->totalForestMemoryUsage * VDO_BLOCK_SIZE),
		       PPRETTY));
  printf("  Slab Reference Count Usage: %s\n",
	 getSizeString((vdo_get_saved_reference_count_size(vdoInfo->userDataBlocks) *
			VDO_BLOCK_SIZE), PPRETTY));
}

static void printVDOVolumeCharacteristics(struct vdoInfo *vdoInfo)
{
  printf("Volume Characteristics:\n");
  printf("  Blocksize: %s\n", getSizeString(VDO_BLOCK_SIZE, PPRETTY));
  printf("  Physical Blocks: %s\n", getSizeString(vdoInfo->physicalBlocks,
						  PPRETTY));
  printf("  Logical Blocks: %s\n", getSizeString(vdoInfo->physicalBlocks,
						 PPRETTY));
  printf("  Slab Size: %s\n", getSizeString(vdoInfo->slabBlockCount, PPRETTY));
  printf("  Slab Count: %ld\n", vdoInfo->slabCount);
  printf("  Index Memory: %s\n", vdoInfo->indexMemorySize);
  printf("  Sparse: %d\n", vdoInfo->sparseIndex);
}	 

static void printVDOMetaData(struct vdoInfo *vdoInfo)
{
  printf("VDO Meta Data:\n");
  printf("  Superblock Size: %s\n",
	 getSizeString((DEFAULT_SUPERBLOCK * VDO_BLOCK_SIZE), PPRETTY));
  printf("  Geometery Block Size: %s\n",
	 getSizeString((DEFAULT_GEOMETRY_BLOCK * VDO_BLOCK_SIZE), PPRETTY));
  printf("  VDO Blockmap Tree Root Count: %d\n", DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  printf("  Slab Summary Size: %s\n",
	 getSizeString((VDO_SLAB_SUMMARY_BLOCKS * VDO_BLOCK_SIZE), PPRETTY));
  printf("  Recovery Journal Size: %s\n",
	 getSizeString((DEFAULT_VDO_RECOVERY_JOURNAL_SIZE * VDO_BLOCK_SIZE),
		       PPRETTY));
  printf("  UDS Index Size: %s\n", getSizeString((vdoInfo->udsIndexSize *
						  VDO_BLOCK_SIZE), PPRETTY));
  printf("  Total BlockMap Pages: %s\n",
         getSizeString((vdoInfo->totalBlockMapPages * VDO_BLOCK_SIZE),
		       PPRETTY));
  printf("  Slab Reference Count Usage: %s\n",
         getSizeString((vdo_get_saved_reference_count_size(vdoInfo->userDataBlocks) *
			VDO_BLOCK_SIZE),PPRETTY));
  printf("  Slab Jouranl Usage: %s\n",
         getSizeString((vdoInfo->slabCount * DEFAULT_VDO_SLAB_JOURNAL_SIZE *
			VDO_BLOCK_SIZE), PPRETTY));
}

static void printVDOInfo(struct vdoInfo *vdoInfo)
{
  printVDOInputParameters(vdoInfo);
  printVDOStorageUsage(vdoInfo);
  printVDOMemoryUsage(vdoInfo);
  printVDOVolumeCharacteristics(vdoInfo);
  printVDOMetaData(vdoInfo);
}

/**********************************************************************/
static void usage(void)
{
  fprintf(stderr,
          "Usage: vdoCalculateSize [--blockMapCacheSize=size]\n"
          "                        [--indexMemorySize=GB]\n"
          "                        [--logicalSize=B]\n"
          "                        [--physicalSize=B]\n"
          "                        [--slabBits=GB]\n"
          "                        [--sparseIndex]\n\n");
  fprintf(stderr,
	  "DESCRIPTION\n"
          "  Calculate VDO space and memory usage.\n"
          "\n"
	  "OPTIONS:"
	  "\n"
          "  --blockMapCacheSize=size     block map cache size\n"
	  "\n"
          "  --indexMemorySize=<gigabytes>\n"
          "    Specify the amount of memory, in gigabytes, to devote to the\n"
          "    index. Accepted options are .25, .5, .75, and all positive\n"
          "    integers.\n"
          "\n"
          "  --logicalSize=GB             VDO logical size\n"
          "\n"
          "  --physicalSize=GB            VDO physical size\n"
          "\n"
	  "  --slabBits                   slabBits\n"
          "    Set the free space allocator's slab size to 2^<bits> 4 KB blocks.\n"
          "    <bits> must be a value between 4 and 23 (inclusive), corresponding\n"
          "    to a slab size between 128 KB and 32 GB. The default value is 19\n"
          "    which results in a slab size of 2 GB. This allocator manages the\n"
          "    space VDO uses to store user data.\n"
          "    The maximum number of slabs in the system is 8192, so this value\n"
          "    determines the maximum physical size of a VDO volume. One slab is\n"
          "    the minimum amount by which a VDO volume can be grown. Smaller\n"
          "    slabs also increase the potential for parallelism if the device\n"
          "    has multiple physical threads. Therefore, this value should be set\n"
          "    as small as possible, given the eventual maximal size of the\n"
          "    volume.\n"
          "\n"
          "  --sparseIndex                Default false\n"
          "\n");
  exit(1);
}

static void parseArgs(int argc, char **argv, struct vdoInfo *vdoInfo)
{
  int result;
  enum { OPT_BLOCKMAPCACHE_SIZE = 'A', OPT_INDEXMEMORY_SIZE, OPT_HUMANREADABLE,
	 OPT_LOGICAL_SIZE, OPT_PHYSICAL_SIZE, OPT_SLABBITS, OPT_SPARSEINDEX};
  struct option options[] = {
    {"blockMapCacheSize", required_argument, NULL, OPT_BLOCKMAPCACHE_SIZE},
    {"indexMemorySize",   required_argument, NULL, OPT_INDEXMEMORY_SIZE},
    {"humanReadable",     no_argument,       NULL, OPT_HUMANREADABLE},
    {"logicalSize",       required_argument, NULL, OPT_LOGICAL_SIZE},
    {"physicalSize",      required_argument, NULL, OPT_PHYSICAL_SIZE},
    {"slabBits",          required_argument, NULL, OPT_SLABBITS},
    {"sparseIndex",       no_argument,       NULL, OPT_SPARSEINDEX},
    {NULL,                0,                 NULL, 0},
  };
  int opt;

  while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
    case OPT_BLOCKMAPCACHE_SIZE:
      result = parseSize(optarg, true, &vdoInfo->blockMapCacheSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_INDEXMEMORY_SIZE:
      vdoInfo->indexMemorySize = optarg;
      break;
    case OPT_LOGICAL_SIZE:
      result = parseSize(optarg, true, &vdoInfo->logicalSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_HUMANREADABLE:
      vdoInfo->humanReadable = 1;
      break;
    case OPT_PHYSICAL_SIZE:
      result = parseSize(optarg, true, &vdoInfo->physicalSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_SLABBITS:
      result = parseUInt(optarg, MIN_SLAB_BITS, MAX_VDO_SLAB_BITS,
			 &(vdoInfo->slabBits));
      if (result != VDO_SUCCESS) {
        warnx("invalid slab bits, must be %u-%u",
              MIN_SLAB_BITS, MAX_VDO_SLAB_BITS);
        usage();
      }
      break;
    case OPT_SPARSEINDEX:
      vdoInfo->sparseIndex = 1;
      break;
    default:
      usage();
      break;
    }
  }
  if (optind < argc) {
    usage();
  }
}

/**********************************************************************/
int main(int argc, char **argv)
{
  struct vdoInfo vdoInfo= {
    .blockMapCacheSize = DEFAULT_BLOCKMAP_CACHE_SIZE,
    .indexMemorySize = "1G",
    .logicalSize = 0,
    .physicalSize = 0,
    .sparseIndex = 0,
    .slabBits = DEFAULT_SLAB_BITS,
    .totalBlockMapLeaves = 0,
    .humanReadable = 0,
  };

  parseArgs(argc, argv, &vdoInfo);
  calculateVDOInfo(&vdoInfo);
  printVDOInfo(&vdoInfo);
  return 0;
}
