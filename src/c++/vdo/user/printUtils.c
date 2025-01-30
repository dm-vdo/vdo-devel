/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#include "printUtils.h"

static const u64 KB = 1024;
static const u64 MB = 1024 * KB;
static const u64 GB = 1024 * MB;
static const u64 TB = 1024 * GB;
static const u64 PB = 1024 * TB;

#define PRINTSIZE 20
static char printString[PRINTSIZE];

char *getSizeString(u64 size, int humanReadable)
{
  memset(printString, 0, PRINTSIZE);
  if (humanReadable) {
    setReadablePrintString(size);
  } else {
    sprintf(printString, "%ld", size);
  }
  return printString;
}

void setReadablePrintString(u64 size)
{
  long double readableSize = 0;
  if ((readableSize = (long double)size/PB) > 1) {
    sprintf(printString, "%.2LfP", readableSize);
  } else if ((readableSize = (long double)size/TB) > 1){
    sprintf(printString, "%.2LfT", (long double)readableSize);
  } else if ((readableSize = (long double)size/GB) > 1){
    sprintf(printString, "%.2LfG", readableSize);
  } else if ((readableSize = (long double)size/MB) > 1){
    sprintf(printString, "%.2LfM", (long double)readableSize);
  } else if ((readableSize = (long double)size/KB) > 1){
    sprintf(printString, "%.2LfK", (long double)readableSize);
  } else {
    sprintf(printString, "%.2LfB", (long double)size);
  }
}
