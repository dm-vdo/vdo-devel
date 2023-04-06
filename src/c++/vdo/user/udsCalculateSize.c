/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "errors.h"
#include "logger.h"
#include "string-utils.h"

#include "encodings.h"
#include "status-codes.h"
#include "types.h"

#include "parseUtils.h"
#include "vdoConfig.h"

static const char usageString[] = " [--help] [options...]";

static const char helpString[] =
  "%s - calculate index size \n"
  "\n"
  "SYNOPSIS\n"
  "  %s [options]\n"
  "\n"
  "DESCRIPTION\n"
  "  calculateIndexSize determines the number of 4k blocks which will be\n"
  "  used by a UDS index with the specified parameters.\n"
  "\n"
  "OPTIONS\n"
  "\n"
  "    --help\n"
  "       Print this help message and exit.\n"
  "\n"
  "    --uds-memory-size=<gigabytes>\n"
  "       Specify the amount of memory, in gigabytes, to devote to the\n"
  "       index. Accepted options are .25, .5, .75, and all positive\n"
  "       integers.\n"
  "\n"
  "    --uds-sparse\n"
  "       Specify whether or not to use a sparse index.\n"
  "\n";

// N.B. the option array must be in sync with the option string.
static struct option options[] = {
  { "help",            no_argument,       NULL, 'h' },
  { "uds-memory-size", required_argument, NULL, 'm' },
  { "uds-sparse",      no_argument,       NULL, 's' },
  { NULL,              0,                 NULL,  0  },
};
static char optionString[] = "hm:s";

static void usage(const char *progname, const char *usageOptionsString)
{
  errx(1, "Usage: %s%s\n", progname, usageOptionsString);
}

int main(int argc, char *argv[])
{
  static char errBuf[UDS_MAX_ERROR_MESSAGE_SIZE];

  int result = vdo_register_status_codes();
  if (result != VDO_SUCCESS) {
    errx(1, "Could not register status codes: %s",
         uds_string_error(result, errBuf, UDS_MAX_ERROR_MESSAGE_SIZE));
  }

  UdsConfigStrings configStrings;
  memset(&configStrings, 0, sizeof(configStrings));

  int c;
  while ((c = getopt_long(argc, argv, optionString, options, NULL)) != -1) {
    switch (c) {
    case 'h':
      printf(helpString, argv[0], argv[0]);
      exit(0);
      break;

    case 'm':
      configStrings.memorySize = optarg;
      break;

    case 's':
      configStrings.sparse = "1";
      break;

    default:
      usage(argv[0], usageString);
      break;
    };
  }

  if (optind != argc) {
    usage(argv[0], usageString);
  }

  char errorBuffer[UDS_MAX_ERROR_MESSAGE_SIZE];

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

  printf("%llu", (unsigned long long) indexBlocks);
  exit(0);
}

