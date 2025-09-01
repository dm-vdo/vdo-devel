/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef LINUX_OVERFLOW_H
#define LINUX_OVERFLOW_H

#include <stdint.h>

static inline size_t size_mul(size_t a, size_t b)
{
	if ((b != 0) && (a > (SIZE_MAX / b)))
		return SIZE_MAX;
	return a * b;
}

static inline size_t size_add(size_t a, size_t b)
{
	if (a > (SIZE_MAX - b))
		return SIZE_MAX;
	return a + b;
}

#define struct_size(PTR, MEMBER, COUNT) \
	size_add(sizeof(*(PTR)), size_mul((COUNT), sizeof((PTR)->MEMBER[0])))

#endif /* LINUX_OVERFLOW_H */
