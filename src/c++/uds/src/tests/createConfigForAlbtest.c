// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
struct configuration *createConfigForAlbtest(int argc, const char **argv)
{
  struct uds_parameters params
    = createUdsParametersForAlbtest(argc, argv);
  struct configuration *config;
  UDS_ASSERT_SUCCESS(uds_make_configuration(&params, &config));
  return config;
}

/**********************************************************************/
struct uds_parameters createUdsParametersForAlbtest(int          argc,
                                                    const char **argv)
{
  bool small  = true;
  bool sparse = false;
  int i;
  for (i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--large") == 0) {
      small = false;
    } else if (strcmp(argv[i], "--small") == 0) {
      small = true;
    } else if (strcmp(argv[i], "--sparse") == 0) {
      sparse = true;
    } else {
      CU_ASSERT(0);
    }
  }

  struct uds_parameters params = {
    .memory_size = (small ? UDS_MEMORY_CONFIG_256MB : 1),
    .sparse = sparse,
  };
  return params;
}
