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

/* .h file in src/c++/vdo/user */
#include "parseUtils.h"
#include "blockMapUtils.h"

/* .h files in src/c++/vdo/base */
#include "constants.h"
#include "status-codes.h"
#include "types.h"
#include "vdoConfig.h"

static const u64 KB = 1024;
static const u64 MB = 1024 * KB;
static const u64 GB = 1024 * MB;

#define CHAPTER_PER_GIG             1024
#define DEFAULT_BLOCKMAP_CACHE_SIZE 32768
#define DEFAULT_GEOMETRY_BLOCK      1
#define DEFAULT_SLAB_SUMMARY_SIZE   64
#define DEFAULT_SUPERBLOCK_SIZE     1

enum {
  MIN_SLAB_BITS        =  4,
  DEFAULT_SLAB_BITS    = 19,
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

static int systemBlock(void)
{
  return DEFAULT_SUPERBLOCK_SIZE +
         DEFAULT_GEOMETRY_BLOCK +
         DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT +
         DEFAULT_SLAB_SUMMARY_SIZE +
         DEFAULT_VDO_RECOVERY_JOURNAL_SIZE;
}

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

static u64 getForestMemoryUsage(u64 logicalBlocks, u64 totalBlockMapPages)
{
 u64 totalBlockMapLeaves = vdo_compute_block_map_page_count(logicalBlocks);
 return totalBlockMapPages - totalBlockMapLeaves;
}

static u64 getSlabBlockCount(unsigned int slabBits)
{
  double blockCount = pow(2, slabBits);
  block_count_t numberOfBlocks = (block_count_t) blockCount;
  return numberOfBlocks;
}

static u64 getUDSIndexSize(char *memorySize, int sparse)
{
  int result;
  UdsConfigStrings configStrings;
  char errorBuffer[VDO_MAX_ERROR_MESSAGE_SIZE];

  memset(&configStrings, 0, sizeof(configStrings));
  if (sparse)
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
  For UDS dense index, there is 1024 chapters for every GB of memory.
  Dedup window size is index memory size multiple by the amount
  of memory in GB.
 */
static u64 getDedupWindowSize(char *indexMemorySize)
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
      errx(result, "parseInt failed: getDedupWindowSize");
    }
    return number * CHAPTER_PER_GIG;
  }
}

static void printVDOInfo(u64 physicalBlock, u64 logicalBlock, int sparse,
			 int slabSize, u64 slabCount, char * indexMemorySize)
{
  printf("Blocksize: %d\n", VDO_BLOCK_SIZE);
  printf("VDO:\n");
  printf("  Physical Block: %ld\n", physicalBlock);
  printf("  Logical Block: %ld\n", logicalBlock);
  printf("  Sparse: %d\n", sparse);
  printf("  Slab Size: %d\n", slabSize);
  printf("  Slab Count: %ld\n", slabCount);
  printf("  Index Memory: %s\n", indexMemorySize);
}

static void printMetaDataInfo(u64 totalBlockMapPage, u64 slabReferenceCountUsage,
			      u64 slabJouralUsage, u64 udsIndexSize)
{
  printf("VDO Constants(4K block):\n");
  printf("  Superblock Size: %d\n", DEFAULT_SUPERBLOCK_SIZE);
  printf("  Geometery Block Size: %d\n", DEFAULT_GEOMETRY_BLOCK);
  printf("  VDO Blockmap Tree Root Count: %d\n", DEFAULT_VDO_BLOCK_MAP_TREE_ROOT_COUNT);
  printf("  Slab Summary Size: %d\n", DEFAULT_SLAB_SUMMARY_SIZE);
  printf("  Recovery Journal Size: %d\n", DEFAULT_VDO_RECOVERY_JOURNAL_SIZE);
  printf("VDO meta data:\n");
  printf("  UDS Index Size: %ldMB\n", (udsIndexSize * VDO_BLOCK_SIZE) / MB);
  printf("  Total BlockMap Pages: %ldMB\n",
          (totalBlockMapPage * VDO_BLOCK_SIZE) / MB);
  printf("  Slab Reference Count Usage: %ldMB\n",
          (slabReferenceCountUsage  * VDO_BLOCK_SIZE) / MB);
  printf("  Slab Jouranl Usage: %ldMB\n",
          (slabJouralUsage  * VDO_BLOCK_SIZE) / MB);
}

static void printUsage(u64 physicalSize, u64 logicalSize,
		       u64 usableSpace, u64 dedupWindowSize)
{
  printf("VDO Usage:\n");
  printf("  Physical Size: %ldGB\n", physicalSize / GB);
  printf("  Logical Size: %ldGB\n", logicalSize / GB);
  printf("  Usable Size: %ldGB\n", usableSpace / GB);
  printf("  Dedup Window: %ldGB\n", dedupWindowSize);
}

static void printMemoryUsage(u64 blockMapCacheSize, u64 totalForestMemoryUsage,
			     u64 slabReferenceCountUsage)
{
  printf("In Memory Usage:\n");
  printf("  Block map cache: %ldMB\n",
           (blockMapCacheSize * VDO_BLOCK_SIZE) / MB);
  printf("  Forest Memory Usage: %ldMB\n",
           (totalForestMemoryUsage * VDO_BLOCK_SIZE) / MB);
  printf("  Slab Reference Count Usage: %ldMB\n",
	   (slabReferenceCountUsage  * VDO_BLOCK_SIZE) / MB);
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

/**********************************************************************/
int main(int argc, char **argv)
{
  char *indexMemorySize = NULL;
  int result = 0, sparseIndex = 0, threadCount = 0;
  unsigned int slabBits = DEFAULT_SLAB_BITS;

  u64 blockMapCacheSize = DEFAULT_BLOCKMAP_CACHE_SIZE;
  u64 logicalSize = 0, physicalSize = 0;

  enum { OPT_BMCS = 'A', OPT_IMS,
         OPT_LS, OPT_PS, OPT_SB, OPT_SI, OPT_TC};

  struct option options[] = {
    {"blockMapCacheSize", required_argument, NULL, OPT_BMCS},
    {"indexMemorySize",   required_argument, NULL, OPT_IMS},
    {"logicalSize",       required_argument, NULL, OPT_LS},
    {"physicalSize",      required_argument, NULL, OPT_PS},
    {"slabBits",          required_argument, NULL, OPT_SB},
    {"sparseIndex",       no_argument,       NULL, OPT_SI},
    {"threadCount",       required_argument, NULL, OPT_TC},
    {NULL,                0,                 NULL,  0 },
  };

  int opt;
  while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
    case OPT_BMCS:
      result = parseSize(optarg, true, &blockMapCacheSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_IMS:
      indexMemorySize = optarg;
      break;
    case OPT_LS:
      result = parseSize(optarg, true, &logicalSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_PS:
      result = parseSize(optarg, true, &physicalSize);
      if (result != VDO_SUCCESS) {
        usage();
      }
      break;
    case OPT_SB:
      result = parseUInt(optarg, MIN_SLAB_BITS, MAX_VDO_SLAB_BITS, &slabBits);
      if (result != VDO_SUCCESS) {
        warnx("invalid slab bits, must be %u-%u",
              MIN_SLAB_BITS, MAX_VDO_SLAB_BITS);
        usage();
      }
      break;
    case OPT_SI:
      sparseIndex = 1;
      break;
    case OPT_TC:
      result = parseInt(optarg, &threadCount);
      break;
    default:
      usage();
      break;
    }
  }

  if (optind < argc) {
    usage();
  }

  u64 dedupWindowSize = getDedupWindowSize(indexMemorySize);
  if (sparseIndex) {
    dedupWindowSize *= 10;
  }
  u64 udsIndexSize = getUDSIndexSize(indexMemorySize, sparseIndex);

  int totalSystemBlock = systemBlock() + udsIndexSize;

  u64 physicalBlock = physicalSize / VDO_BLOCK_SIZE;
  u64 logicalBlock = logicalSize / VDO_BLOCK_SIZE;
  u64 userDataBlock = physicalBlock - totalSystemBlock;

  u64 slabBlockCount = getSlabBlockCount(slabBits);
  u64 slabCount = userDataBlock / slabBlockCount;
  u64 totalSlabJouranl = slabCount * DEFAULT_VDO_SLAB_JOURNAL_SIZE;
  u64 totalReferenceCount = vdo_get_saved_reference_count_size(userDataBlock);
  u64 totalBlockMapPages = getBlockMapPages(logicalBlock);
  u64 totalUsableSpace = (userDataBlock - totalBlockMapPages -
			  totalReferenceCount - totalSlabJouranl) * VDO_BLOCK_SIZE;
  u64 totalForestMemoryUsage = getForestMemoryUsage(logicalBlock,
						    totalBlockMapPages);
  printVDOInfo(physicalBlock, logicalBlock, sparseIndex,
	       slabBlockCount, slabCount, indexMemorySize);
  printMetaDataInfo(totalBlockMapPages, totalReferenceCount,
		    totalSlabJouranl, udsIndexSize);
  printUsage(physicalSize, logicalSize, totalUsableSpace, dedupWindowSize);
  printMemoryUsage(blockMapCacheSize, totalForestMemoryUsage,
		   totalReferenceCount);
  return 0;
}
