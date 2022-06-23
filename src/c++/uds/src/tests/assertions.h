/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef ASSERTIONS_H
#define ASSERTIONS_H

#include "compiler.h"
#include "cpu.h"
#include "errors.h"
#include "string-utils.h"
#ifdef __KERNEL__
#include "logger.h"
#include "uds-threads.h"
#else
#include <assert.h>
#include "processManager.h"
#endif

#ifdef __KERNEL__
#define CU_COMPLAIN(PRED) \
  uds_log_error("\n%s:%d: %s: %s: ", __FILE__, __LINE__, __func__, PRED)
#else
#define CU_COMPLAIN(PRED) \
  fprintf(stderr, "\n%s:%d: %s: %s: ", __FILE__, __LINE__, __func__, PRED)
#endif

#ifdef __KERNEL__
#define CU_MESSAGE(...) \
  uds_log_error(__VA_ARGS__)
#else
#define CU_MESSAGE(...)           \
  do {                            \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n");        \
  } while (0)
#endif

/**
 * Print the error message for a system error
 *
 * @param string  The stringized value passed to the CU assertion macro
 * @param value   The integer error code resulting from the stringized code
 **/
#ifdef __KERNEL__
void cuErrorMessage(const char *string, int value);
#else
static INLINE void cuErrorMessage(const char *string, int value)
{
  char buf[UDS_MAX_ERROR_MESSAGE_SIZE];
  const char *errmsg = uds_string_error_name(value, buf, sizeof(buf));
  fprintf(stderr, "%s: %s (%d)\n", string, errmsg, value);
}
#endif

/**
 * An assertion has triggered, so try to die cleanly
 **/
__attribute__((noreturn))
static INLINE void cuDie(void)
{
#ifdef __KERNEL__
  uds_thread_exit();
#else
  killChildren();
  assert(0);
#endif
}

#define CU_FAIL(...)         \
  do {                       \
    CU_COMPLAIN("CU_FAIL");  \
    CU_MESSAGE(__VA_ARGS__); \
    cuDie();                 \
  } while (0)

#define CU_ASSERT(expr)                 \
  do {                                  \
    if (!(expr)) {                      \
      CU_COMPLAIN("CU_ASSERT");         \
      CU_MESSAGE("%s", __STRING(expr)); \
      cuDie();                          \
    }                                   \
  } while (0)

#define CU_ASSERT_TRUE(val) CU_ASSERT(val)

#define CU_ASSERT_FALSE(val) CU_ASSERT(!(val))

#define CU_ASSERT_GENERIC_PREDICATE(test, predicate, a, b, type, format, ...) \
  do {                                                              \
    type _a = (a);                                                  \
    type _b = (b);                                                  \
    if (!(predicate)) {                                             \
      CU_COMPLAIN(test);                                            \
      CU_MESSAGE("Assertion failed!\n"                              \
                 "\t(%s) vs (%s)\n\t" format, #a, #b, __VA_ARGS__); \
      cuDie();                                                      \
    }                                                               \
  } while (0)

#define CU_ASSERT_EQUAL_TYPE(test, a, b, type, format)                  \
  CU_ASSERT_GENERIC_PREDICATE(test, _a == _b, a, b,                     \
                              type,                                     \
                              "(%" format " vs %" format ")",           \
                              _a, _b)

#ifdef __KERNEL__
#define CU_ASSERT_EQUAL(a, b)                   \
  CU_ASSERT_EQUAL_TYPE("CU_ASSERT_EQUAL", a, b, uint64_t, "llu")
#else
#define CU_ASSERT_EQUAL(a, b)                   \
  CU_ASSERT_EQUAL_TYPE("CU_ASSERT_EQUAL", a, b, uintmax_t, "ju")
#endif

#define CU_ASSERT_PTR_EQUAL(a, b)               \
  CU_ASSERT_EQUAL_TYPE("CU_ASSERT_PTR_EQUAL", a, b, const void *, "p")

#define CU_ASSERT_NOT_EQUAL(a, b)               \
  CU_ASSERT((a) != (b))

#define CU_ASSERT_STRING_EQUAL(a, b)                            \
  CU_ASSERT_GENERIC_PREDICATE("CU_ASSERT_STRING_EQUAL",         \
                              strcmp(_a, _b) == 0,              \
                              a, b,                             \
                              const char *, "('%s' vs '%s')",   \
                              _a, _b);

#define CU_ASSERT_STRING_NOT_EQUAL(a, b)                        \
  CU_ASSERT_GENERIC_PREDICATE("CU_ASSERT_STRING_NOT_EQUAL",     \
                              strcmp(_a, _b) != 0,              \
                              a, b,                             \
                              const char *, "('%s' vs '%s')",   \
                              _a, _b);

#define CU_ASSERT_SUBSTRING_EQUAL(a, b, length)                 \
  CU_ASSERT_GENERIC_PREDICATE("CU_ASSERT_SUBSTRING_EQUAL",      \
                              strncmp(_a, _b, (length)) == 0,   \
                              a, b,                             \
                              const char *,                     \
                              "('%.*s' vs '%.*s')",             \
                              (length), _a, (length), _b);

#define CU_ASSERT_CONTAINS_SUBSTRING(haystack, needle)          \
  CU_ASSERT_GENERIC_PREDICATE("CU_ASSERT_CONTAINS_SUBSTRING",   \
                              strstr(_a, _b) != NULL,           \
                              haystack, needle,                 \
                              const char *,                     \
                              "('%s' not found in '%s')",       \
                              _b, _a);

#define UDS_ASSERT_SUCCESS(result)       \
  do {                                   \
    int _r = (result);                   \
    if (_r != UDS_SUCCESS) {             \
      CU_COMPLAIN("UDS_ASSERT_SUCCESS"); \
      cuErrorMessage(#result, _r);       \
      cuDie();                           \
    }                                    \
  } while (0)

#define UDS_ASSERT_ERROR(error1, result) \
  do {                                   \
    int _r = (result);                   \
    if (_r != (error1)) {                \
      CU_COMPLAIN("UDS_ASSERT_ERROR");   \
      cuErrorMessage(#result, _r);       \
      cuDie();                           \
    }                                    \
  } while (0)

#define UDS_ASSERT_ERROR2(error1, error2, result) \
  do {                                            \
    int _r = (result);                            \
    if ((_r != (error1)) && (_r != (error2))) {   \
      CU_COMPLAIN("UDS_ASSERT_ERROR");            \
      cuErrorMessage(#result, _r);                \
      cuDie();                                    \
    }                                             \
  } while (0)

#define UDS_ASSERT_ERROR3(error1, error2, error3, result)           \
  do {                                                              \
    int _r = (result);                                              \
    if ((_r != (error1)) && (_r != (error2)) && (_r != (error3))) { \
      CU_COMPLAIN("UDS_ASSERT_ERROR");                              \
      cuErrorMessage(#result, _r);                                  \
      cuDie();                                                      \
    }                                                               \
  } while (0)

#define UDS_ASSERT_ERROR4(error1, error2, error3, error4, result) \
  do {                                                            \
    int _r = (result);                                            \
    if ((_r != (error1)) && (_r != (error2))                      \
        && (_r != (error3)) && (_r != (error4))) {                \
      CU_COMPLAIN("UDS_ASSERT_ERROR");                            \
      cuErrorMessage(#result, _r);                                \
      cuDie();                                                    \
    }                                                             \
  } while (0)

#ifdef __KERNEL__
#define UDS_ASSERT_KERNEL_SUCCESS(result)       \
  do {                                          \
    void *_r = (result);                        \
    if (IS_ERR(_r)) {                           \
      CU_COMPLAIN("UDS_ASSERT_KERNEL_SUCCESS"); \
      cuErrorMessage(#result, -PTR_ERR(_r));    \
      cuDie();                                  \
    }                                           \
  } while (0)
#else
#define UDS_ASSERT_SYSTEM_CALL(result)       \
  do {                                       \
    int _r = (result);                       \
    if (_r == -1) {                          \
      CU_COMPLAIN("UDS_ASSERT_SYSTEM_CALL"); \
      cuErrorMessage(#result, errno);        \
      cuDie();                               \
    }                                        \
  } while (0)
#endif

#define UDS_ASSERT_EQUAL_BYTES(a, b, size)                              \
  do {                                                                  \
    const void   *_a = (a);                                             \
    const void   *_b = (b);                                             \
    const size_t  _s = (size);                                          \
    if (memcmp(_a, _b, _s) != 0) {                                      \
      char buf[50];                                                     \
      CU_COMPLAIN("UDS_ASSERT_EQUAL_BYTES");                            \
      CU_MESSAGE("Assertion failed!\n"                                  \
                 "\t(%s) vs (%s) for %zd bytes\n"                       \
                 "\t(bytes differ: %s)",                                \
                 #a, #b, _s,                                            \
                 displayByteDifferences(buf, sizeof(buf), _a, _b, _s)); \
      cuDie();                                                          \
    }                                                                   \
  } while (0)

#define UDS_ASSERT_NOT_EQUAL_BYTES(first, second, length)  \
  CU_ASSERT_FALSE(memcmp((first), (second), (length)) == 0)

#define UDS_ASSERT_BLOCKNAME_EQUAL(first, second) \
  UDS_ASSERT_EQUAL_BYTES((first), (second), sizeof(struct uds_chunk_name));

#define UDS_ASSERT_BLOCKNAME_NOT_EQUAL(first, second) \
  UDS_ASSERT_NOT_EQUAL_BYTES((first), (second), sizeof(struct uds_chunk_name));

#define UDS_ASSERT_BLOCKDATA_EQUAL(first, second) \
  UDS_ASSERT_EQUAL_BYTES((first), (second), sizeof(struct uds_chunk_data));

#define CU_ASSERT_PTR_NOT_NULL(ptr)          \
  do {                                       \
    if ((ptr) == NULL) {                     \
      CU_COMPLAIN("CU_ASSERT_PTR_NOT_NULL"); \
      CU_MESSAGE("%s", __STRING(ptr));       \
      cuDie();                               \
    }                                        \
  } while (0)

#define CU_ASSERT_PTR_NULL(ptr)          \
  do {                                   \
    if ((ptr) != NULL) {                 \
      CU_COMPLAIN("CU_ASSERT_PTR_NULL"); \
      CU_MESSAGE("%s", __STRING(ptr));   \
      cuDie();                           \
    }                                    \
  } while (0)

#define CU_ASSERT_DOUBLE_EQUAL(actual, expected, tolerance)     \
  CU_ASSERT_TRUE((actual) >= (expected) - (tolerance)           \
              && (actual) <= (expected) + (tolerance))

/**
 * Display a description of the differences between two byte arrays of
 * equal length.
 *
 * @param buf           The output buffer.
 * @param bufSize       The size of buf.
 * @param a             The first byte array to compare.
 * @param b             The second byte array to compare.
 * @param size          The size of the byte arrays.
 *
 * @return buf, properly terminated with " ..." as the end of the string
 *         if insufficient space for more information was provided
 **/
const char *displayByteDifferences(char       *buf,
                                   size_t      bufSize,
                                   const byte *a,
                                   const byte *b,
                                   size_t      size);

/**********************************************************************/
static INLINE void assertCacheAligned(const volatile void *address)
{
  CU_ASSERT_EQUAL(0, (uintptr_t) address & (CACHE_LINE_BYTES - 1));
}

#endif /* ASSERTIONS_H */
