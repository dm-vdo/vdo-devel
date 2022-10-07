/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef LINUX_ATOMIC_H
#define LINUX_ATOMIC_H

#include "compiler.h"
#include "type-defs.h"

// The atomic interfaces are chosen to exactly match those interfaces defined
// by the Linux kernel.  The rest of this file is the matching user-mode
// implementation.

typedef struct {
  int32_t value;
} atomic_t;

typedef struct {
  int64_t value;
} atomic64_t;

#define ATOMIC_INIT(i)  { (i) }

/*****************************************************************************
 * Beginning of the barrier methods.
 *****************************************************************************/

/**
 * Stop GCC from moving memory operations across a point in the instruction
 * stream.  This is how the kernel uses this method.
 **/
static inline void barrier(void)
{
  /*
   * asm volatile cannot be removed, and the memory clobber tells the
   * compiler not to move memory accesses past the asm.  We don't
   * actually need any instructions issued on x86_64, as synchronizing
   * instructions are ordered with respect to both loads and stores,
   * with some irrelevant-to-us exceptions.
   */
  __asm__ __volatile__("" : : : "memory");
}

/**
 * Provide a memory barrier.
 *
 * Generate a full memory fence for the compiler and CPU. Load and store
 * operations issued before the fence will not be re-ordered with operations
 * issued after the fence.
 *
 * We also use this method in association with the __sync builtins. In earlier
 * versions of GCC (at least through 4.6), the __sync operations didn't
 * actually act as the memory barriers the compiler documentation says they
 * should. Even as of GCC 8, it looks like the Linux kernel developers
 * disagree with the compiler developers as to what constitutes a barrier at
 * least on s390x, where the kernel uses explicit barriers after certain
 * atomic operations and GCC does not.
 *
 * Rather than investigate the current status of barriers in GCC (which is an
 * architecture-specific issue), and since in user mode the performance of
 * these operations is not critical, we can afford to be cautious and insert
 * extra barriers, until such time as we have more time to investigate and
 * gain confidence in the current state of GCC barriers.
 **/
static inline void smp_mb(void)
{
#if defined __x86_64__
  /*
   * X86 full fence. Supposedly __sync_synchronize() will do this, but
   * either the GCC documentation is a lie or GCC is broken.
   *
   * FIXME: http://blogs.sun.com/dave/entry/atomic_fetch_and_add_vs says
   * atomicAdd of zero may be a better way to spell this on current CPUs.
   */
  __asm__ __volatile__("mfence" : : : "memory");
#elif defined __aarch64__
  __asm__ __volatile__("dmb ish" : : : "memory");
#elif defined __s390__
  __asm__ __volatile__("bcr 14,0" : : : "memory");
#elif defined __PPC__
  __asm__ __volatile__("sync" : : : "memory");
#else
#error "no fence defined"
#endif
}

/**
 * Provide a read memory barrier.
 *
 * Memory load operations that precede this fence will be prevented from
 * changing order with any that follow this fence, by either the compiler or
 * the CPU. This can be used to ensure that the load operations accessing the
 * fields of a structure are not re-ordered so they actually take effect before
 * a pointer to the structure is resolved.
 **/
static inline void smp_rmb(void)
{
#if defined __x86_64__
  // The implementation on x86 is more aggressive than necessary.
  __asm__ __volatile__("lfence" : : : "memory");
#elif defined __aarch64__
  __asm__ __volatile__("dmb ishld" : : : "memory");
#elif defined __s390__
  __asm__ __volatile__("bcr 14,0" : : : "memory");
#elif defined __PPC__
  __asm__ __volatile__("lwsync" : : : "memory");
#else
#error "no fence defined"
#endif
}

/**
 * Provide a write memory barrier.
 *
 * Memory store operations that precede this fence will be prevented from
 * changing order with any that follow this fence, by either the compiler or
 * the CPU. This can be used to ensure that the store operations initializing
 * the fields of a structure are not re-ordered so they actually take effect
 * after a pointer to the structure is published.
 **/
static inline void smp_wmb(void)
{
#if defined __x86_64__
  // The implementation on x86 is more aggressive than necessary.
  __asm__ __volatile__("sfence" : : : "memory");
#elif defined __aarch64__
  __asm__ __volatile__("dmb ishst" : : : "memory");
#elif defined __s390__
  __asm__ __volatile__("bcr 14,0" : : : "memory");
#elif defined __PPC__
  __asm__ __volatile__("lwsync" : : : "memory");
#else
#error "no fence defined"
#endif
}

/**
 * Provide a memory barrier before an atomic read-modify-write operation
 * that does not imply one.
 **/
static inline void smp_mb__before_atomic(void)
{
#if defined(__x86_64__) || defined(__s390__)
  // Atomic operations are already serializing on x86 and s390
  barrier();
#else
  smp_mb();
#endif
}

/**
 * Provide a memory barrier after an atomic read-modify-write operation
 * that does not imply one.
 **/
static inline void smp_mb__after_atomic(void)
{
#if defined(__x86_64__) || defined(__s390__)
  // Atomic operations are already serializing on x86 and s390
  barrier();
#else
  smp_mb();
#endif
}

/*****************************************************************************
 * Beginning of the methods for defeating compiler optimization.
 *****************************************************************************/

#ifdef TEST_INTERNAL
/*
 * We borrowed this code from the Linux kernel sources, and are unsure how to
 * legally ship it for user mode usage.  Therefore the usage of TEST_INTERNAL.
 *
 * The READ_ONCE and WRITE_ONCE interfaces are chosen to exactly match those
 * interfaces defined by the Linux kernel.  This is the matching user-mode
 * implementation.
 *
 * Prevent the compiler from merging or refetching reads or writes.  The
 * compiler is also forbidden from reordering successive instances of READ_ONCE
 * and WRITE_ONCE, but only when the compiler is aware of some particular
 * ordering.  One way to make the compiler aware of ordering is to put the two
 * invocations of READ_ONCE or WRITE_ONCE in different C statements.
 *
 * These two macros will also work on aggregate data types like structs or
 * unions.  If the size of the accessed data type exceeds the word size of the
 * machine (e.g., 32 bits or 64 bits) READ_ONCE() and WRITE_ONCE() will fall
 * back to memcpy().  There's at least two memcpy()s: one for the
 * __builtin_memcpy() and then one for the macro doing the copy of variable -
 * '__u' allocated on the stack.
 *
 * Their major use case is ensuring that the compiler does not fold, spindle,
 * or otherwise mutilate accesses that either do not require ordering or that
 * interact with an explicit memory barrier or atomic instruction that provides
 * the required ordering.
 */

#define READ_ONCE(x)                                 \
  __extension__ ({                                   \
    union { __typeof__(x) __val; char __c[1]; } __u; \
    read_once_size(&(x), __u.__c, sizeof(x));        \
    __u.__val;                                       \
  })

#define WRITE_ONCE(x, val)                          \
  __extension__ ({                                  \
    union { __typeof__(x) __val; char __c[1]; } __u \
      = { .__val = (__typeof__(x)) (val) };         \
    write_once_size(&(x), __u.__c, sizeof(x));      \
    __u.__val;                                      \
  })

static inline void read_once_size(const volatile void *p, void *res, int size)
{
  switch (size) {
  case 1: *(byte *)res     = *(const volatile byte *)p;     break;
  case 2: *(uint16_t *)res = *(const volatile uint16_t *)p; break;
  case 4: *(uint32_t *)res = *(const volatile uint32_t *)p; break;
  case 8: *(uint64_t *)res = *(const volatile uint64_t *)p; break;
  default:
    barrier();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    __builtin_memcpy((void *)res, (const void *)p, size);
#pragma GCC diagnostic pop
    barrier();
  }
  /*
   * FIXME: Some platforms such as Alpha may need an additional barrier
   * here. See https://lkml.org/lkml/2019/11/8/1021
   */
}

static inline void write_once_size(volatile void *p, void *res, int size)
{
  switch (size) {
  case 1: *(volatile byte *)p     = *(byte *)res;     break;
  case 2: *(volatile uint16_t *)p = *(uint16_t *)res; break;
  case 4: *(volatile uint32_t *)p = *(uint32_t *)res; break;
  case 8: *(volatile uint64_t *)p = *(uint64_t *)res; break;
  default:
    barrier();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    __builtin_memcpy((void *)p, (const void *)res, size);
#pragma GCC diagnostic pop
    barrier();
  }
}
#else /* TEST_INTERNAL */
#define READ_ONCE(x)        (x)
#define WRITE_ONCE(x, val)  ((x) = (val))
#endif /* TEST_INTERNAL */

/*****************************************************************************
 * Beginning of the 32 bit atomic support.
 *****************************************************************************/

/*
 * As noted above, there are a lot of explicit barriers here, in places
 * where we need barriers. Ideally, GCC should just Get It Right on all the
 * platforms. But there have been bugs in the past, and it looks like there
 * might be one still (in GCC 8) at least on s390 (no bug report filed yet),
 * and researching it may take more time than we have available before we have
 * to ship. It also requires manual inspection for each platform, as there's
 * no good general way to test whether the compiler gets the barriers correct.
 */

/**
 * Add a signed int to a 32-bit atomic variable.  The addition is atomic, but
 * there are no memory barriers implied by this method.
 *
 * @param delta  the value to be added to (or subtracted from) the variable
 * @param atom   a pointer to the atomic variable
 **/
static inline void atomic_add(int delta, atomic_t *atom)
{
  /*
   * According to the kernel documentation, the addition is atomic, but there
   * are no memory barriers implied by this method.
   *
   * The x86 implementation does do memory barriers.
   */
  __sync_add_and_fetch(&atom->value, delta);
}

/**
 * Add a signed int to a 32-bit atomic variable.  The addition is properly
 * atomic, and there are memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return the new value of the atom after the add operation
 **/
static inline int atomic_add_return(int delta, atomic_t *atom)
{
  smp_mb();
  int result = __sync_add_and_fetch(&atom->value, delta);
  smp_mb();
  return result;
}

/**
 * Compare and exchange a 32-bit atomic variable.  The operation is properly
 * atomic and performs a memory barrier.
 *
 * @param atom  a pointer to the atomic variable
 * @param old   the value that must be present to perform the swap
 * @param new   the value to be swapped for the required value
 *
 * @return the old value
 **/
static inline int atomic_cmpxchg(atomic_t *atom, int old, int new)
{
  smp_mb();
  int result = __sync_val_compare_and_swap(&atom->value, old, new);
  smp_mb();
  return result;
}

/**
 * Increment a 32-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 **/
static inline void atomic_inc(atomic_t *atom)
{
  /*
   * According to the kernel documentation, the addition is atomic, but there
   * are no memory barriers implied by this method.
   *
   * The x86 implementation does do memory barriers.
   */
  __sync_add_and_fetch(&atom->value, 1);
}

/**
 * Increment a 32-bit atomic variable.  The addition is properly atomic, and
 * there are memory barriers.
 *
 * @param atom  a pointer to the atomic variable
 *
 * @return the new value of the atom after the increment
 **/
static inline long atomic_inc_return(atomic_t *atom)
{
  return atomic_add_return(1, atom);
}


/**
 * Decrement a 32-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 **/
static inline void atomic_dec(atomic_t *atom)
{
  /*
   * According to the kernel documentation, the subtraction is atomic, but
   * there are no memory barriers implied by this method.
   *
   * The x86 implementation does do memory barriers.
   */
  __sync_sub_and_fetch(&atom->value, 1);
}

/**
 * Read a 32-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 **/
static inline int atomic_read(const atomic_t *atom)
{
  return READ_ONCE(atom->value);
}

/**
 * Read a 32-bit atomic variable, with an acquire memory barrier.
 *
 * @param atom  a pointer to the atomic variable
 **/
static inline int atomic_read_acquire(const atomic_t *atom)
{
  int value = READ_ONCE(atom->value);
  smp_mb();
  return value;
}

/**
 * Set a 32-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 * @param value  the value to set it to
 **/
static inline void atomic_set(atomic_t *atom, int value)
{
  atom->value = value;
}

/**
 * Set a 32-bit atomic variable, with a release memory barrier.
 *
 * @param atom   a pointer to the atomic variable
 * @param value  the value to set it to
**/
static inline void atomic_set_release(atomic_t *atom, int value)
{
  smp_mb();
  atomic_set(atom, value);
}

/*****************************************************************************
 * Beginning of the 64 bit atomic support.
 *****************************************************************************/

/**
 * Add a signed long to a 64-bit atomic variable.  The addition is atomic, but
 * there are no memory barriers implied by this method.
 *
 * @param delta  the value to be added to (or subtracted from) the variable
 * @param atom   a pointer to the atomic variable
 **/
static inline void atomic64_add(long delta, atomic64_t *atom)
{
  /*
   * According to the kernel documentation, the addition is atomic, but there
   * are no memory barriers implied by this method.
   *
   * The x86 implementation does do memory barriers.
   */
  __sync_add_and_fetch(&atom->value, delta);
}

/**
 * Add a signed long to a 64-bit atomic variable.  The addition is properly
 * atomic, and there are memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return the new value of the atom after the add operation
 **/
static inline long atomic64_add_return(long delta, atomic64_t *atom)
{
  smp_mb();
  long result = __sync_add_and_fetch(&atom->value, delta);
  smp_mb();
  return result;
}

/**
 * Compare and exchange a 64-bit atomic variable.  The operation is properly
 * atomic and performs a memory barrier.
 *
 * @param atom  a pointer to the atomic variable
 * @param old   the value that must be present to perform the swap
 * @param new   the value to be swapped for the required value
 *
 * @return the old value
 **/
static inline long atomic64_cmpxchg(atomic64_t *atom, long old, long new)
{
  smp_mb();
  long result = __sync_val_compare_and_swap(&atom->value, old, new);
  smp_mb();
  return result;
}

/**
 * Increment a 64-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 **/
static inline void atomic64_inc(atomic64_t *atom)
{
  /*
   * According to the kernel documentation, the addition is atomic, but there
   * are no memory barriers implied by this method.
   *
   * The x86 implementation does do memory barriers.
   */
  __sync_add_and_fetch(&atom->value, 1);
}

/**
 * Increment a 64-bit atomic variable.  The addition is properly atomic, and
 * there are memory barriers.
 *
 * @param atom  a pointer to the atomic variable
 *
 * @return the new value of the atom after the increment
 **/
static inline long atomic64_inc_return(atomic64_t *atom)
{
  return atomic64_add_return(1, atom);
}

/**
 * Read a 64-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 **/
static inline long atomic64_read(const atomic64_t *atom)
{
  return READ_ONCE(atom->value);
}

/**
 * Read a 64-bit atomic variable, with an acquire memory barrier.
 *
 * @param atom  a pointer to the atomic variable
 **/
static inline long atomic64_read_acquire(const atomic64_t *atom)
{
  long value = READ_ONCE(atom->value);
  smp_mb();
  return value;
}

/**
 * Set a 64-bit atomic variable, without any memory barriers.
 *
 * @param atom   a pointer to the atomic variable
 * @param value  the value to set it to
 **/
static inline void atomic64_set(atomic64_t *atom, long value)
{
  atom->value = value;
}

/**
 * Set a 64-bit atomic variable, with a release memory barrier.
 *
 * @param atom   a pointer to the atomic variable
 * @param value  the value to set it to
**/
static inline void atomic64_set_release(atomic64_t *atom, long value)
{
  smp_mb();
  atomic64_set(atom, value);
}

/*****************************************************************************
 * Generic exchange support.
 *****************************************************************************/

/*
 * Exchange a location's value atomically, with a full memory barrier.
 *
 * The location is NOT an "atomic*_t" type, but any primitive type for which
 * an exchange can be done atomically. (This varies by processor, but
 * generally a word-sized or pointer-sized value is supported.) As this uses a
 * type-generic compiler interface, it must be implemented as a macro.
 *
 * @param PTR     a pointer to the location to be updated
 * @param NEWVAL  the new value to be stored
 *
 * @return the old value
 */
#define xchg(PTR,NEWVAL)                                                \
  __extension__ ({                                                      \
    __typeof__(*(PTR)) __xchg_result;                                   \
    __typeof__(*(PTR)) __xchg_new_value = (NEWVAL);                     \
    smp_mb();  /* paranoia, for old gcc bugs */                         \
    __xchg_result = __atomic_exchange_n((PTR), __xchg_new_value,        \
                                        __ATOMIC_SEQ_CST);              \
    smp_mb();  /* more paranoia */                                      \
    __xchg_result;                                                      \
  })

#endif /* LINUX_ATOMIC_H */
