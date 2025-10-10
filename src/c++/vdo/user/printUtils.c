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

void getSizeString(u64 size, int humanReadable, char *printString)
{
  memset(printString, 0, PRINTSTRINGSIZE);
  if (humanReadable) {
    setReadablePrintString(size, printString);
  } else {
    sprintf(printString, "%ld", size);
  }
}

void setReadablePrintString(u64 size, char *printString)
{
  if (size > PB) {
    sprintf(printString, "%.2fP", (double)size / PB);
  } else if (size > TB) {
    sprintf(printString, "%.2fT", (double)size / TB);
  } else if (size > GB) {
    sprintf(printString, "%.2fG", (double)size / GB);
  } else if (size > MB) {
    sprintf(printString, "%.2fM", (double)size / MB);
  } else if (size / KB) {
    sprintf(printString, "%.2fK", (double)size / KB);
  } else {
    sprintf(printString, "%.2fB", (double)size);
  }
}
