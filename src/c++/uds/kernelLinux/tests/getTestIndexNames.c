// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
const char *const *getTestIndexNames(void)
{
  static const char *const names[2] = {
    "/dev/zubenelgenubi_scratch",
    NULL
  };
  return names;
}

/**********************************************************************/
const char *const *getTestMultiIndexNames(void)
{
  static const char *const names[3] = {
    "/dev/zubenelgenubi_scratch-0",
    "/dev/zubenelgenubi_scratch-1",
    NULL,
  };
  return names;
}
