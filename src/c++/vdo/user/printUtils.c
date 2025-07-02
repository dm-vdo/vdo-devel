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
  u64 readableSize = 0;
  if ((readableSize = size/PB) > 0) {
    sprintf(printString, "%ldP", readableSize);
  } else if ((readableSize = size/TB) > 0){
    sprintf(printString, "%ldT", readableSize);
  } else if ((readableSize = size/GB) > 0){
    sprintf(printString, "%ldG", readableSize);
  } else if ((readableSize = size/MB) > 0){
    sprintf(printString, "%ldM", readableSize);
  } else if ((readableSize = size/KB) > 0){
    sprintf(printString, "%ldK", readableSize);
  } else {
    sprintf(printString, "%ldB", size);
  }
}
