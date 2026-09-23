/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef LINUX_UNALIGNED_H
#define LINUX_UNALIGNED_H

#include <asm/byteorder.h>
#include <linux/types.h>

/*
 * Unaligned-safe pointer types. __attribute__((aligned(1))) tells GCC and
 * UBSAN that a pointer to this type may have any alignment, suppressing the
 * misalignment check without hiding the dereference from the sanitizer.
 * We dereference through these typedefs and then call the value form of the
 * byte-order macros (__le32_to_cpu rather than __le32_to_cpup) so that no
 * unaligned pointer ever escapes into a system header.
 */
typedef __le16 __attribute__((aligned(1))) unaligned_le16;
typedef __le32 __attribute__((aligned(1))) unaligned_le32;
typedef __le64 __attribute__((aligned(1))) unaligned_le64;
typedef __be16 __attribute__((aligned(1))) unaligned_be16;
typedef __be32 __attribute__((aligned(1))) unaligned_be32;
typedef __be64 __attribute__((aligned(1))) unaligned_be64;

/* Type safe comparison macros, similar to the ones in linux/minmax.h. */

/*
 * If pointers to types are comparable (without dereferencing them and
 * potentially causing side effects) then types are the same.
 */
 #define __typecheck(x, y) \
	(!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))

/* 
 * Hack for VDO to replace use of the kernel's __is_constexpr() in __cmp_ macros.
 * VDO cannot use __is_constexpr() due to it relying on a GCC extension to allow sizeof(void).
 */
#define __constcheck(x, y) \
	(__builtin_constant_p(x) && __builtin_constant_p(y))

/* It takes two levels of macro expansion to compose the unique temp names. */
#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

/* Defined in linux/minmax.h */
#define __cmp_op_min <
#define __cmp_op_max >

#define __cmp(op, x, y)	((x) __cmp_op_##op (y) ? (x) : (y))

#define __cmp_once(op, x, y, unique_x, unique_y) \
	__extension__({                          \
		typeof(x) unique_x = (x);        \
		typeof(y) unique_y = (y);        \
		__cmp(op, unique_x, unique_y);   \
	})

#define __careful_cmp(op, x, y)                            \
	__builtin_choose_expr(                             \
		(__typecheck(x, y) && __constcheck(x, y)), \
		__cmp(op, x, y),                           \
		__cmp_once(op, x, y, __UNIQUE_ID(x_), __UNIQUE_ID(y_)))

#define min(x, y) __careful_cmp(min, x, y)
#define max(x, y) __careful_cmp(max, x, y)

/* Defined in linux/minmax.h */
#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

/* Defined in linux/math.h */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Defined in asm/unaligned.h */
static inline uint16_t get_unaligned_le16(const void *p)
{
	return __le16_to_cpu(*(const unaligned_le16 *)p);
}

static inline uint32_t get_unaligned_le32(const void *p)
{
	return __le32_to_cpu(*(const unaligned_le32 *)p);
}

static inline uint64_t get_unaligned_le64(const void *p)
{
	return __le64_to_cpu(*(const unaligned_le64 *)p);
}

static inline uint16_t get_unaligned_be16(const void *p)
{
	return __be16_to_cpu(*(const unaligned_be16 *)p);
}

static inline uint32_t get_unaligned_be32(const void *p)
{
	return __be32_to_cpu(*(const unaligned_be32 *)p);
}

static inline uint64_t get_unaligned_be64(const void *p)
{
	return __be64_to_cpu(*(const unaligned_be64 *)p);
}

static inline void put_unaligned_le16(uint16_t val, void *p)
{
	*(unaligned_le16 *)p = __cpu_to_le16(val);
}

static inline void put_unaligned_le32(uint32_t val, void *p)
{
	*(unaligned_le32 *)p = __cpu_to_le32(val);
}

static inline void put_unaligned_le64(uint64_t val, void *p)
{
	*(unaligned_le64 *)p = __cpu_to_le64(val);
}

static inline void put_unaligned_be16(uint16_t val, void *p)
{
	*(unaligned_be16 *)p = __cpu_to_be16(val);
}

static inline void put_unaligned_be32(uint32_t val, void *p)
{
	*(unaligned_be32 *)p = __cpu_to_be32(val);
}

static inline void put_unaligned_be64(uint64_t val, void *p)
{
	*(unaligned_be64 *)p = __cpu_to_be64(val);
}


/* Native byte order */
#define get_unaligned(ptr) __extension__({			\
	typeof(*(ptr)) __attribute__((aligned(1))) *__gu_p =	\
		(typeof(*(ptr)) __attribute__((aligned(1))) *)(ptr);\
	*__gu_p;						\
})

#define put_unaligned(val, ptr) __extension__({			\
	typeof(*(ptr)) __attribute__((aligned(1))) *__pu_p =	\
		(typeof(*(ptr)) __attribute__((aligned(1))) *)(ptr);\
	*__pu_p = (val);					\
})

#endif /* LINUX_UNALIGNED_H */
