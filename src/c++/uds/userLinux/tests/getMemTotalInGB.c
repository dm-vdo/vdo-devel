/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "assertions.h"
#include "testPrototypes.h"

size_t getMemTotalInGB(void)
{
  size_t size = 0;
  FILE *f = fopen("/proc/meminfo", "r");
  CU_ASSERT_PTR_NOT_NULL(f);
  char lineBuf[100];
  while (fgets(lineBuf, sizeof(lineBuf), f) != NULL) {
    if (sscanf(lineBuf, "MemTotal: %zu kB\n", &size) == 1) {
      break;
    }
  }
  fclose(f);
  return size >> 20;
}
