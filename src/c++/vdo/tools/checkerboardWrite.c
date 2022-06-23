/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * Copy some of the data from the source to the destination in a regular
 * pattern. Starting from the source offset, the program will copy a number
 * of sectors equal to the --write-sectors argument, then skip over a number
 * of sectors equal to the --skip-sectors argument. This process is repeated
 * --iterations times. If the source is not long enough, the program will
 * terminate with an error. The destination will be created if it does not yet
 * exist.
 *
 * $Id$
 */

#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { SECTOR_SIZE = 512 };

/**********************************************************************/
static void usage(void)
{
  fprintf(stderr,
          "Usage:  checkerboardWrite [--source=<path>] [--source-offset=N]\n"
          "                          [--destination=<path>]\n"
          "                          [--destination-offset=N]\n"
          "                          [--write-sectors=N] [--skip-sectors=N]\n"
          "                          [--iterations=N] [--skip-first]\n");
  fprintf(stderr,
          "Write data from the source to the destination, skipping sections\n"
          "according to a regular pattern."
          "\n"
          "\t--source=<path>         pathname of the source file\n"
          "\n"
          "\t--source-offset=N       offset into the source file to start\n"
          "\t                        reading\n"
          "\n"
          "\t--destination=<path>    pathname of the destination file\n"
          "\n"
          "\t--destination-offset=N  offset into the destination file to\n"
          "\t                        start writing\n"
          "\n"
          "\t--write-sectors=N       number of sectors to write together\n"
          "\n"
          "\t--skip-sectors=N        number of sectors to skip together\n"
          "\n"
          "\t--iterations=N          number of write-skip cycles to complete\n"
          "\n"
          "\t--skip-first            if true, skip first, then write\n");
  exit(1);
}

/**********************************************************************/
static int parseInt(const char *arg)
{
  char *leftover;
  long value = strtol(arg, &leftover, 0);
  if (leftover[0] != '\0') {
    errx(2, "Invalid number");
  }
  if (value != (int) value) {
    errx(2, "Numeric value too large");
  }
  return (int) value;
}

/**********************************************************************/
static void transferData(const char *sourcePath,
                         int         sourceOffset,
                         const char *destinationPath,
                         int         destinationOffset,
                         int         writeSectors,
                         int         skipSectors,
                         int         iterationCount,
                         bool        skipFirst) {

  int writeLength = writeSectors * SECTOR_SIZE;
  int skipLength  = skipSectors * SECTOR_SIZE;

  int sourceFileDescriptor = open(sourcePath, (O_RDONLY | O_DIRECT));
  if (sourceFileDescriptor < 0) {
    errx(1, "Cannot open source location %s", sourcePath);
  }

  int destinationFileDescriptor = open(destinationPath,
                                       (O_WRONLY | O_DIRECT | O_CREAT),
                                       0666);
  if (destinationFileDescriptor < 0) {
    errx(1, "Cannot open destination location %s", destinationPath);
  }

  if (lseek(sourceFileDescriptor, sourceOffset, SEEK_SET) < 0) {
    errx(1, "Cannot seek to source offset %d", sourceOffset);
  }

  if (lseek(destinationFileDescriptor, destinationOffset, SEEK_SET) < 0) {
    errx(1, "Cannot seek to destination offset %d", destinationOffset);
  }

  if (skipFirst) {
    if (lseek(sourceFileDescriptor, skipLength, SEEK_CUR) < 0) {
      errx(1, "Cannot skip %d source bytes", skipLength);
    }

    if (lseek(destinationFileDescriptor, skipLength, SEEK_CUR) < 0) {
      errx(1, "Cannot skip %d destination bytes", skipLength);
    }
  }

  void *buffer;
  int result = posix_memalign(&buffer, SECTOR_SIZE, writeLength);
  if (result != 0) {
    errx(1, "Could not allocate aligned buffer for copying");
  }

  for (int i = 0; i < iterationCount; i++) {
    ssize_t bytesRead = read(sourceFileDescriptor, buffer, writeLength);
    if (bytesRead != writeLength) {
      errx(1, "Could not read from %s", sourcePath);
    }
    ssize_t bytesWritten = write(destinationFileDescriptor, buffer,
                                 writeLength);
    if (bytesWritten != writeLength) {
      errx(1, "Could not write to %s", destinationPath);
    }

    if (lseek(sourceFileDescriptor, skipLength, SEEK_CUR) < 0) {
      errx(1, "Cannot skip %d source bytes", skipLength);
    }

    if (lseek(destinationFileDescriptor, skipLength, SEEK_CUR) < 0) {
      errx(1, "Cannot skip %d destination bytes", skipLength);
    }
  }

  if (close(sourceFileDescriptor) < 0) {
    errx(1, "Cannot close source location %s", sourcePath);
  }

  if (close(destinationFileDescriptor) < 0) {
    errx(1, "Cannot close destination location %s", destinationPath);
  }
}

/**********************************************************************/
int main(int argc, char **argv)
{
  char  *sourcePath        = NULL;
  char  *destinationPath   = NULL;
  off_t  sourceOffset      = 0;
  off_t  destinationOffset = 0;
  int    writeSectors      = 0;
  int    skipSectors       = 0;
  int    iterationCount    = 8;
  bool   skipFirst         = false;

  struct option options[] = {
    {"source",             required_argument, NULL, 'f'},
    {"source-offset",      required_argument, NULL, 'o'},
    {"destination",        required_argument, NULL, 'F'},
    {"destination-offset", required_argument, NULL, 'O'},
    {"skip-first",         no_argument,       NULL, 'k'},
    {"iterations",         required_argument, NULL, 'i'},
    {"write-sectors",      required_argument, NULL, 's'},
    {"skip-sectors",       required_argument, NULL, 'S'},
    {NULL,                 0,                 NULL,  0 },
  };
  int opt;
  while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
    case 'f':  // --source=<path>
      sourcePath = optarg;
      break;
    case 'F':  // --destination=<path>
      destinationPath = optarg;
      break;
    case 'o':  // --source-offset=<path>
      sourceOffset = parseInt(optarg);
      break;
    case 'O':  // --destination-offset=<path>
      destinationOffset = parseInt(optarg);
      break;
    case 'k': // --skip-first
      skipFirst = true;
      break;
    case 'i': // --iterations=<count>
      iterationCount = parseInt(optarg);
      break;
    case 's': // --write-sectors=<count>
      writeSectors = parseInt(optarg);
      break;
    case 'S': // --skip-sectors=<count>
      skipSectors = parseInt(optarg);
      break;
    default:
      usage();
      break;
    }
  }

  if (optind < argc) {
    usage();
  }

  transferData(sourcePath, sourceOffset, destinationPath, destinationOffset,
               writeSectors, skipSectors, iterationCount, skipFirst);
  return 0;
}
