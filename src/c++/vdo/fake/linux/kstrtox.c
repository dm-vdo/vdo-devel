/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 *
 */
#include <linux/kstrtox.h>

#include <linux/types.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/**
 * kstrtoint - convert a string to a signed int
 * @string: The start of the string. The string must be null-terminated.
 *          The first character may also be a plus sign, but not a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *        given as 0, then the base of the string is automatically detected
 *        with the conventional semantics - If it begins with 0x the number
 *        will be parsed as a hexadecimal (case insensitive), if it otherwise
 *        begins with 0, it will be parsed as an octal number. Otherwise it
 *        will be parsed as a decimal.
 * @result: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 */
int kstrtoint(const char *string, unsigned int base, int *result)
{
  long long tmp;

  /*
   * To mimic the kernel implementation we must exclude options that strtoll
   * supports which the kernel does not.
   *
   * The string must begin with a non-whitespace character which strtoll would
   * skip but the kernel implementation would consider an invalid form.
   */
  if (isspace(string[0])) {
    return -EINVAL;
  }

  /*
   * The kernel auto-detects, if base is 0, the base to use for the number in
   * the same manner as strtoll.
   *
   * The kernel documentation states the largest supported base is 16; this is
   * technically not correct. Rather than attempt to mimic the real behavior we
   * opt to check that the base is not greater than 16 thus supporting a valid
   * but more restricted range of values than the kernel implementation.
   */
  if (base > 16) {
    return -EINVAL;
  }

  tmp = strtoll(string, NULL, base);
  if ((errno == ERANGE) || (tmp != ((int) tmp))) {
    return -ERANGE;
  }

  *result = (int) tmp;
  return 0;
}

/**
 * kstrtouint - convert a string to an unsigned int
 * @string: The start of the string. The string must be null-terminated.
 *          The first character may also be a plus sign, but not a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *        given as 0, then the base of the string is automatically detected
 *        with the conventional semantics - If it begins with 0x the number
 *        will be parsed as a hexadecimal (case insensitive), if it otherwise
 *        begins with 0, it will be parsed as an octal number. Otherwise it
 *        will be parsed as a decimal.
 * @result: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 */
int kstrtouint(const char *string, unsigned int base, unsigned int *result)
{
  long long tmp;

  /*
   * To mimic the kernel implementation we must exclude options that strtoll
   * supports which the kernel does not.
   *
   * The string must not begin with a '-' and it must begin with a
   * non-whitespace character which strtoll would skip but the kernel
   * implementation would consider an invalid form.
   */
  if ((string[0] == '-') || isspace(string[0])) {
    return -EINVAL;
  }

  /*
   * The kernel auto-detects, if base is 0, the base to use for the number in
   * the same manner as strtoll.
   *
   * The kernel documentation states the largest supported base is 16; this is
   * technically not correct. Rather than attempt to mimic the real behavior we
   * opt to check that the base is not greater than 16 thus supporting a valid
   * but more restricted range of values than the kernel implementation.
   */
  if (base > 16) {
    return -EINVAL;
  }

  tmp = strtoll(string, NULL, base);
  if ((errno == ERANGE) || (tmp != ((unsigned int) tmp))) {
    return -ERANGE;
  }

  *result = (unsigned int) tmp;
  return 0;
}

/**
 * kstrtoull - convert a string to an unsigned long long
 * @string: The start of the string. The string must be null-terminated, and may
 *          also include a single newline before its terminating null. The first
 *          character may also be a plus sign, but not a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *        given as 0, then the base of the string is automatically detected with
 *        the conventional semantics - If it begins with 0x the number will be
 *        parsed as a hexadecimal (case insensitive), if it otherwise begins
 *        with 0, it will be parsed as an octal number. Otherwise it will be
 *        parsed as a decimal.
 * @result: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 */
int kstrtoull(const char *string, unsigned int base, uint64_t *result)
{
  unsigned long long tmp;
  char *endPtr;

  if ((string[0] == '-') || isspace(string[0])) {
    return -EINVAL;
  }

  if (base > 16) {
    return -EINVAL;
  }

  tmp = strtoull(string, &endPtr, base);
  if (tmp == 0) {
    return -EINVAL;
  }

  if ((errno == ERANGE) || (endPtr == NULL) || (tmp != (uint64_t) tmp)) {
    return -ERANGE;
  }

  *result = tmp;
  return 0;
}
