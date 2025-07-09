/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef PRINT_UTILS_H
#define PRINT_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

char *getSizeString(u64 size, int humanReadable);
void setReadablePrintString(u64 size);

#endif // PRINT_UTILS_H
