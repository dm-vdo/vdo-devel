/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "testUtils.h"

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "assertions.h"

/**********************************************************************/
static char *makeNameTemplate(const char *what)
{
  size_t n = strlen(what);
  if (n == 0) {
    err(1, "no temporary info specified");
  }

  bool absolute = (what[0] == '/');
  bool dirname  = (what[n - 1] == '/');
  bool plain    = !strchr(what, '/');

  static const char *tmpDir = "/tmp/";
  static const char *albTmp   = "AlbTmp";

  char *newName;
  UDS_ASSERT_SUCCESS(vdo_alloc_sprintf(__func__,
                                       &newName, "%s%s%s%s%s.XXXXXX",
                                       absolute         ? ""     : tmpDir,
                                       plain            ? ""     : what,
                                       dirname || plain ? albTmp : "",
                                       plain            ? "."    : "",
                                       plain            ? what   : ""));
  return newName;
}

/**********************************************************************/
char *makeTempFileName(const char *what)
{
  char *name = makeNameTemplate(what);
  int fd = mkstemp(name);
  if (fd < 0) {
    err(1, "can't create temporary %s file name", what);
  }
  // Caller doesn't want a file descriptor.
  close(fd);
  // Caller doesn't want an existing file.
  unlink(name);
  return name;
}
