// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "string-utils.h"

/**********************************************************************/
__printf(5, 6)
static char *addMore(char       *buf,
                     char       *pos,
                     char       *end,
                     const char *sep,
                     const char *fmt,
                     ...)
{
  const char *s = ((sep != NULL) && (pos > buf)) ? sep : "";
  if (pos < end) {
    va_list ap;
    va_start(ap, fmt);
    if (*s) {
      pos = uds_append_to_buffer(pos, end, "%s", s);
    }
    pos = uds_v_append_to_buffer(pos, end, fmt, ap);
    va_end(ap);
    if (pos == end) {
      while (*s && (pos > buf) &&
             (*pos != *s ||
              ((size_t) (end - pos) < strlen(s) + sizeof("...")))) {
        --pos;
      }
      uds_append_to_buffer(pos, end, "%s%s", s,  "...");
      pos = end;
    }
  }
  return pos;
}

/**********************************************************************/
const char *displayByteDifferences(char       *buf,
                                   size_t      bufSize,
                                   const byte *a,
                                   const byte *b,
                                   size_t      size)
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
        if (i > x+1) {
          bp = addMore(buf, bp, be, ", ", "%d-%d", x, i - 1);
        } else {
          bp = addMore(buf, bp, be, ", ", "%d", x);
        }
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
