// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
struct uds_configuration *makeDenseConfiguration(uds_memory_config_size_t memGB)
{
  struct uds_parameters params = {
    .memory_size = memGB,
  };
  struct uds_configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  return config;
}
