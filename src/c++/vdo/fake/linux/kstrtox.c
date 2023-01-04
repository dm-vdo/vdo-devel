/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 *
 */
#include <linux/kstrtox.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

/**
 * kstrtouint - convert a string to an unsigned int
 *
 * Mimics, as closely as reasonable, the kernel-provided version.
 *
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
 * The sole reason for the existence of this function is to allow writing
 * product source such that checkpatch doesn't complain about not using
 * kstrtouint for single format character reads of unsigned ints.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 * Return code must be checked.
 */
int __must_check kstrtouint(const char *string,
                            unsigned int base,
                            unsigned int *result)
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
   * technically not correct.  The current kernel (v6.0.7) does only support
   * individual digits from the hexadecimal set but makes no check that the
   * specified base is no greater than 16.  Consequently one can have a base
   * greater than 16 as long as the individual digits are not outside the
   * hexadecimal set.  Rather than attempt to mimic this we opt to check that
   * the base is not greater than 16 thus supporting a valid but more
   * restricted range of values than the kernel implementation.
   */
  if (base > 16) {
    return -EINVAL;
  }

  tmp = strtoll(string, NULL, base);
  if (tmp == 0) {
    return -EINVAL;
  }

  if ((errno == ERANGE) || (tmp != ((unsigned int) tmp))) {
    return -ERANGE;
  }

  *result = (unsigned int) tmp;
  return 0;
}

/**
 * kstrtoull - convert a string to an unsigned long long
 * @s: The start of the string. The string must be null-terminated, and may also
 *  include a single newline before its terminating null. The first character
 *  may also be a plus sign, but not a minus sign.
 * @base: The number base to use. The maximum supported base is 16. If base is
 *  given as 0, then the base of the string is automatically detected with the
 *  conventional semantics - If it begins with 0x the number will be parsed as a
 *  hexadecimal (case insensitive), if it otherwise begins with 0, it will be
 *  parsed as an octal number. Otherwise it will be parsed as a decimal.
 * @res: Where to write the result of the conversion on success.
 *
 * Returns 0 on success, -ERANGE on overflow and -EINVAL on parsing error.
 * Preferred over simple_strtoull(). Return code must be checked.
 */
int kstrtoull(const char *s, unsigned int base, uint64_t *result)
{
  unsigned long long tmp;
  char *endPtr;

  if ((s[0] == '-') || isspace(s[0])) {
    return -EINVAL;
  }

  if (base > 16) {
    return -EINVAL;
  }

  tmp = strtoull(s, &endPtr, base);
  if (tmp == 0) {
    return -EINVAL;
  }

  if ((errno == ERANGE) || (endPtr == NULL) || (tmp != (uint64_t) tmp)) {
    return -ERANGE;
  }

  *result = tmp;
  return 0;
}
