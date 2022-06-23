/*
 * FOR INTERNAL USE ONLY, DO NOT DISTRIBUTE!!!!
 *
 * Unit test implementation of asm/unaligned.h
 *
 * $Id$
 */
#ifndef ASM_UNALIGNED_H
#define ASM_UNALIGNED_H

#include "types.h"

typedef uint64_t u64;

#define __get_unaligned_t(type, ptr) ({                               \
  const struct { type x; } __packed *__pptr = (typeof(__pptr))(ptr);  \
  __pptr->x;                                                          \
})

#define get_unaligned(ptr) __get_unaligned_t(typeof(*(ptr)), (ptr))

#endif // ASM_UNALIGNED_H
