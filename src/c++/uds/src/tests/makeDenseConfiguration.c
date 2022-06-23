// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
struct configuration *makeDenseConfiguration(uds_memory_config_size_t memGB)
{
  struct uds_parameters params = {
    .memory_size = memGB,
  };
  struct configuration *config;
  UDS_ASSERT_SUCCESS(make_configuration(&params, &config));
  return config;
}
