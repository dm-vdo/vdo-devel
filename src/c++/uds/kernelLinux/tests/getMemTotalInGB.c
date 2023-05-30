// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/mm.h>

#include "testPrototypes.h"

size_t getMemTotalInGB(void)
{
  struct sysinfo si;
  si_meminfo(&si);
  return (size_t) si.totalram * si.mem_unit >> 30;
}
