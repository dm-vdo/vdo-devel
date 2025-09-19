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

#define PRINTSTRINGSIZE 20

void getSizeString(u64 size, int humanReadable, char *printString);
void setReadablePrintString(u64 size, char *printString);

#endif // PRINT_UTILS_H
