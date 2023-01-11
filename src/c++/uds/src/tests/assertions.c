// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "string-utils.h"

/**********************************************************************/
static char *addRange(char       *buf,
                      char       *pos,
                      char       *end,
                      int first,
                      int last)
{
  const char *sep = (pos > buf) ? ", " : "";
  if (pos < end) {
    if (first < last) {
      pos = uds_append_to_buffer(pos, end, "%s%d-%d", sep, first, last);
    } else {
      pos = uds_append_to_buffer(pos, end, "%s%d", sep, first);
    }

    if (pos == end) {
      while ((pos > buf) &&
             (*pos != *sep ||
              ((size_t) (end - pos) < strlen(sep) + sizeof("...")))) {
        --pos;
      }

      uds_append_to_buffer(pos, end, "%s%s", sep,  "...");
      pos = end;
    }
  }

  return pos;
}

/**********************************************************************/
const char *displayByteDifferences(char     *buf,
                                   size_t    bufSize,
                                   const u8 *a,
                                   const u8 *b,
                                   size_t    size)
{
  char *bp = buf, *be = buf + bufSize;

  // -1 means matching, > -1 is index of first mismatch
  int x = -1;
  int i;
  for (i = 0; i < (int) size + 1; ++i) {
    if (((i == (int) size) && (x != -1)) || ((x == -1) != (a[i] == b[i]))) {
      if (x == -1) {
        x = i;
      } else {
        bp = addRange(buf, bp, be, x, i - 1);
        x = -1;
      }
    }
  }
  return buf;
}

#ifdef __KERNEL__
/**********************************************************************/
void cuErrorMessage(const char *string, int value)
{
  // This is inline in userspace. The linux kernel limits us to 400
  // bytes of stack frame, and sadly UDS_MAX_ERROR_MESSAGE_SIZE is
  // nearly a third of that 400 bytes.
  char buf[UDS_MAX_ERROR_MESSAGE_SIZE];
  const char *errmsg = uds_string_error_name(value, buf, sizeof(buf));
  uds_log_error("%s: %s (%d)", string, errmsg, value);
}
#endif
