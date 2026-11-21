/* SPDX-License-Identifier: GPL-2.0 */

#ifndef LINUX_UNALIGNED_H
#define LINUX_UNALIGNED_H

/*
 * This is adapted from the kernel's tools/include/linux/unaligned.h and
 * tools/include/vdso/unaligned.h, with the get/put_unaligned_t helpers
 * inlined here (no separate vdso/ header in this tree) and a few changes
 * to satisfy -pedantic and -Wcast-qual, which aren't used for kernel
 * builds.
 */

#include <asm/byteorder.h>
#include <linux/compiler_attributes.h>
#include <linux/types.h>

/*
 * <asm/byteorder.h> here only provides the double-underscore raw
 * conversions; alias the plain names used below, as
 * include/linux/byteorder/generic.h does for real kernel builds. (There is
 * no tools/include equivalent of generic.h to copy this from.)
 */
#define cpu_to_le16 __cpu_to_le16
#define le16_to_cpu __le16_to_cpu
#define cpu_to_le32 __cpu_to_le32
#define le32_to_cpu __le32_to_cpu
#define cpu_to_le64 __cpu_to_le64
#define le64_to_cpu __le64_to_cpu
#define cpu_to_be16 __cpu_to_be16
#define be16_to_cpu __be16_to_cpu
#define cpu_to_be32 __cpu_to_be32
#define be32_to_cpu __be32_to_cpu
#define cpu_to_be64 __cpu_to_be64
#define be64_to_cpu __be64_to_cpu

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *			       non-scalar types unchanged.
 *
 * From tools/include/linux/compiler_types.h, which is not included here
 * because its own includes (compiler-context-analysis.h, compiler-gcc.h)
 * are not available in this tree.
 */
#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,			\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(char)0,				\
			 __scalar_type_to_expr_cases(char),		\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),		\
			 __scalar_type_to_expr_cases(long),		\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))

/**
 * __get_unaligned_t - read an unaligned value from memory.
 * @type:	the type to load from the pointer.
 * @ptr:	the pointer to load from.
 *
 * Use memcpy to affect an unaligned type sized load avoiding undefined behavior
 * from approaches like type punning that require -fno-strict-aliasing in order
 * to be correct. As type may be const, use __unqual_scalar_typeof to map to a
 * non-const type - you can't memcpy into a const type. The
 * __get_unaligned_ctrl_type gives __unqual_scalar_typeof its required
 * expression rather than type, a pointer is used to avoid warnings about mixing
 * the use of 0 and NULL. The void* cast silences ubsan warnings.
 *
 * The cast to (const void *), rather than (void *) as upstream uses, avoids
 * -Wcast-qual, since memcpy's source argument is already const void *.
 */
#define __get_unaligned_t(type, ptr) __extension__ ({				\
	type *__get_unaligned_ctrl_type __always_unused = NULL;		\
	__unqual_scalar_typeof(*__get_unaligned_ctrl_type) __get_unaligned_val; \
	__builtin_memcpy(&__get_unaligned_val, (const void *)(ptr),		\
			 sizeof(__get_unaligned_val));			\
	__get_unaligned_val;						\
})

/**
 * __put_unaligned_t - write an unaligned value to memory.
 * @type:	the type of the value to store.
 * @val:	the value to store.
 * @ptr:	the pointer to store to.
 *
 * Use memcpy to affect an unaligned type sized store avoiding undefined
 * behavior from approaches like type punning that require -fno-strict-aliasing
 * in order to be correct. The void* cast silences ubsan warnings.
 */
#define __put_unaligned_t(type, val, ptr) do {				\
	type __put_unaligned_val = (val);				\
	__builtin_memcpy((void *)(ptr), &__put_unaligned_val,		\
			 sizeof(__put_unaligned_val));			\
} while (0)

#define get_unaligned(ptr)	__get_unaligned_t(typeof(*(ptr)), (ptr))
#define put_unaligned(val, ptr) __put_unaligned_t(typeof(*(ptr)), (val), (ptr))

static inline u16 get_unaligned_le16(const void *p)
{
	return le16_to_cpu(__get_unaligned_t(__le16, p));
}

static inline u32 get_unaligned_le32(const void *p)
{
	return le32_to_cpu(__get_unaligned_t(__le32, p));
}

static inline u64 get_unaligned_le64(const void *p)
{
	return le64_to_cpu(__get_unaligned_t(__le64, p));
}

static inline void put_unaligned_le16(u16 val, void *p)
{
	__put_unaligned_t(__le16, cpu_to_le16(val), p);
}

static inline void put_unaligned_le32(u32 val, void *p)
{
	__put_unaligned_t(__le32, cpu_to_le32(val), p);
}

static inline void put_unaligned_le64(u64 val, void *p)
{
	__put_unaligned_t(__le64, cpu_to_le64(val), p);
}

static inline u16 get_unaligned_be16(const void *p)
{
	return be16_to_cpu(__get_unaligned_t(__be16, p));
}

static inline u32 get_unaligned_be32(const void *p)
{
	return be32_to_cpu(__get_unaligned_t(__be32, p));
}

static inline u64 get_unaligned_be64(const void *p)
{
	return be64_to_cpu(__get_unaligned_t(__be64, p));
}

static inline void put_unaligned_be16(u16 val, void *p)
{
	__put_unaligned_t(__be16, cpu_to_be16(val), p);
}

static inline void put_unaligned_be32(u32 val, void *p)
{
	__put_unaligned_t(__be32, cpu_to_be32(val), p);
}

static inline void put_unaligned_be64(u64 val, void *p)
{
	__put_unaligned_t(__be64, cpu_to_be64(val), p);
}

static inline u32 __get_unaligned_be24(const u8 *p)
{
	return p[0] << 16 | p[1] << 8 | p[2];
}

static inline u32 get_unaligned_be24(const void *p)
{
	return __get_unaligned_be24(p);
}

static inline u32 __get_unaligned_le24(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16;
}

static inline u32 get_unaligned_le24(const void *p)
{
	return __get_unaligned_le24(p);
}

static inline void __put_unaligned_be24(const u32 val, u8 *p)
{
	*p++ = (val >> 16) & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = val & 0xff;
}

static inline void put_unaligned_be24(const u32 val, void *p)
{
	__put_unaligned_be24(val, p);
}

static inline void __put_unaligned_le24(const u32 val, u8 *p)
{
	*p++ = val & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = (val >> 16) & 0xff;
}

static inline void put_unaligned_le24(const u32 val, void *p)
{
	__put_unaligned_le24(val, p);
}

static inline void __put_unaligned_be48(const u64 val, u8 *p)
{
	*p++ = (val >> 40) & 0xff;
	*p++ = (val >> 32) & 0xff;
	*p++ = (val >> 24) & 0xff;
	*p++ = (val >> 16) & 0xff;
	*p++ = (val >> 8) & 0xff;
	*p++ = val & 0xff;
}

static inline void put_unaligned_be48(const u64 val, void *p)
{
	__put_unaligned_be48(val, p);
}

static inline u64 __get_unaligned_be48(const u8 *p)
{
	return (u64)p[0] << 40 | (u64)p[1] << 32 | (u64)p[2] << 24 |
		p[3] << 16 | p[4] << 8 | p[5];
}

static inline u64 get_unaligned_be48(const void *p)
{
	return __get_unaligned_be48(p);
}

/*
 * The following are not part of linux/unaligned.h; they are included here
 * because this is a convenient place to inject them. They are used by
 * vdo/fake/linux/minmax.h (__careful_cmp) and by code that would otherwise
 * get them from the real linux/minmax.h and linux/math.h, neither of which
 * is available in this tree.
 */

/*
 * __typecheck, __PASTE/__UNIQUE_ID, __cmp_op_min/max, __cmp, __cmp_once,
 * __careful_cmp, min, max, and swap below are from an older version of
 * include/linux/minmax.h (current upstream has since rewritten __careful_cmp
 * in terms of 'auto' and BUILD_BUG_ON_MSG). There is no tools/include
 * equivalent of minmax.h to copy from instead.
 */

/*
 * If pointers to types are comparable (without dereferencing them and
 * potentially causing side effects) then types are the same.
 */
 #define __typecheck(x, y) \
	(!!(sizeof((typeof(x) *)1 == (typeof(y) *)1)))

/*
 * __is_constexpr(x) - Whether x is known at compile time to be a true ISO C
 *		       integer constant expression.
 *
 * From include/linux/compiler.h. This relies on a GCC extension allowing
 * sizeof(void); __extension__ suppresses the resulting -Wpointer-arith
 * warning, which -pedantic enables here but not for kernel builds.
 */
#define __is_constexpr(x) \
	__extension__(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

/* It takes two levels of macro expansion to compose the unique temp names. */
#define ___PASTE(a,b) a##b
#define __PASTE(a,b) ___PASTE(a,b)
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

#define __cmp_op_min <
#define __cmp_op_max >

#define __cmp(op, x, y)	((x) __cmp_op_##op (y) ? (x) : (y))

#define __cmp_once(op, x, y, unique_x, unique_y) \
	__extension__({                          \
		typeof(x) unique_x = (x);        \
		typeof(y) unique_y = (y);        \
		__cmp(op, unique_x, unique_y);   \
	})

#define __careful_cmp(op, x, y)                                             \
	__builtin_choose_expr(                                             \
		(__typecheck(x, y) && __is_constexpr(x) && __is_constexpr(y)), \
		__cmp(op, x, y),                                           \
		__cmp_once(op, x, y, __UNIQUE_ID(x_), __UNIQUE_ID(y_)))

#define min(x, y) __careful_cmp(min, x, y)
#define max(x, y) __careful_cmp(max, x, y)

#define swap(a, b) \
	do { typeof(a) __tmp = (a); (a) = (b); (b) = __tmp; } while (0)

/* From tools/include/linux/math.h. */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#endif /* LINUX_UNALIGNED_H */
