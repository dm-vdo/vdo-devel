/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef ASSERTIONS_H
#define ASSERTIONS_H

#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "errors.h"
#include "indexer.h"
#include "processManager.h"

#ifndef TEST_ERROR_NAME_FUNC
# define TEST_ERROR_NAME_FUNC uds_string_error_name
#endif

#ifndef TEST_DEBUG_DUMP_ACTION
# define TEST_DEBUG_DUMP_ACTION
#endif

#define CU_COMPLAIN_AND_DIE(PRED, ...)                  \
  do {                                                  \
    fprintf(stderr, "\n%s:%d: %s: %s: ",                \
            __FILE__, __LINE__, __func__, PRED);        \
    fprintf(stderr, __VA_ARGS__);                       \
    fprintf(stderr, "\n");                              \
    TEST_DEBUG_DUMP_ACTION;                             \
    killChildren();                                     \
    assert(0);                                          \
  } while (0)

#define CU_FAIL(...) CU_COMPLAIN_AND_DIE("CU_FAIL", __VA_ARGS__)

#define CU_ASSERT(expr)                                       \
  do {                                                        \
    if (!(expr)) {                                            \
      CU_COMPLAIN_AND_DIE("CU_ASSERT", "%s", __STRING(expr)); \
    }                                                         \
  } while (0)

#define CU_ASSERT_TRUE(val) CU_ASSERT((val))

#define CU_ASSERT_FALSE(val) CU_ASSERT(!(val))

#define CU_ASSERT_GENERIC_PREDICATE(test, predicate, a, b, type, format, ...) \
  do {                                                                  \
    type _a = (a);                                                      \
    type _b = (b);                                                      \
    if (!(predicate)) {                                                 \
      CU_COMPLAIN_AND_DIE(test, "Assertion failed!\n"                   \
                          "\t(%s) vs (%s)\n\t" format,                  \
                          #a, #b, __VA_ARGS__);                         \
    }                                                                   \
  } while(0)

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
#endif /* __KERNEL__ */
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

#define UDS_ASSERT_SUCCESS(result)                                           \
  do {                                                                       \
    int _r = (result);                                                       \
    if (_r != UDS_SUCCESS) {                                                 \
      char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];                               \
      const char *errmsg = TEST_ERROR_NAME_FUNC(_r, errbuf, sizeof(errbuf)); \
      CU_COMPLAIN_AND_DIE("UDS_ASSERT_SUCCESS",                              \
                          "%s: %s (%d)", #result, errmsg, _r);               \
    }                                                                        \
  } while(0)

#define UDS_ASSERT_SYSTEM_CALL(result)                            \
  do {                                                            \
    int _r = (result);                                            \
    if (_r != 0) {                                                \
      char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];                    \
      const char *errmsg                                          \
        = TEST_ERROR_NAME_FUNC(errno, errbuf, sizeof(errbuf));    \
      CU_COMPLAIN_AND_DIE("UDS_ASSERT_SYSTEM_CALL",               \
                          "%s: %s (%d)", #result, errmsg, errno); \
    }                                                             \
  } while(0)

#define UDS_ASSERT_EQUAL_BYTES(first, second, length)   \
  CU_ASSERT(memcmp((first), (second), (length)) == 0)

#define UDS_ASSERT_NOT_EQUAL_BYTES(first, second, length)  \
  CU_ASSERT_FALSE(memcmp((first), (second), (length)) == 0)

#define UDS_ASSERT_BLOCKNAME_EQUAL(first, second) \
  UDS_ASSERT_EQUAL_BYTES((first), (second), sizeof(struct uds_record_name));

#define UDS_ASSERT_BLOCKNAME_NOT_EQUAL(first, second) \
  UDS_ASSERT_NOT_EQUAL_BYTES((first), (second), \
                             sizeof(struct uds_record_name));

#define CU_ASSERT_PTR_NOT_NULL(ptr)            \
  CU_ASSERT_TRUE(ptr != NULL)

#define CU_ASSERT_PTR_NULL(ptr)                \
  CU_ASSERT_TRUE(ptr == NULL)

// This uses >= and <=; to deal with infinities, we cannot use > and <.
#define CU_ASSERT_DOUBLE_EQUAL(actual, expected, tolerance)        \
  do {                                                             \
    double _actual    = actual;                                    \
    double _expected  = expected;                                  \
    double _tolerance = tolerance;                                 \
    CU_ASSERT_TRUE(((_actual) >= (_expected) - (_tolerance))       \
                   && ((_actual) <= (_expected) + (_tolerance)));  \
  } while (0)

#define CU_ASSERT_BETWEEN_TYPE(test, value, lower, upper, type, format) \
  do {                                                                  \
    type _v  = (value);                                                 \
    type _lo = (lower);                                                 \
    type _hi = (upper);                                                 \
    if ((_v < _lo) || (_v > _hi)) {                                     \
      CU_COMPLAIN_AND_DIE(test, "Assertion failed!\n"                   \
                          "\t(%s) not in range (%s) through (%s)\n\t"   \
                          "(%" format " vs %" format " - %" format ")", \
                          #value, #lower, #upper, _v, _lo, _hi);        \
    }                                                                   \
  } while(0)

#ifdef __KERNEL__
#define CU_ASSERT_BETWEEN(actual, lowerBound, upperBound)               \
  CU_ASSERT_BETWEEN_TYPE("CU_ASSERT_BETWEEN", actual, lowerBound,       \
                         upperBound, uint64_t, "llu")
#else
#define CU_ASSERT_BETWEEN(actual, lowerBound, upperBound)               \
  CU_ASSERT_BETWEEN_TYPE("CU_ASSERT_BETWEEN", actual, lowerBound,       \
                         upperBound, uintmax_t, "ju")
#endif /* __KERNEL__ */

#endif /* ASSERTIONS_H */
