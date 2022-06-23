/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef FILE_ASSERTIONS_H
#define FILE_ASSERTIONS_H

#include "assertions.h"
#include "fileUtils.h"

#define UDS_ASSERT_FILE_EXISTS(path)                    \
  do {                                                  \
    bool exists = false;                                \
    UDS_ASSERT_SUCCESS(file_exists((path), &exists));   \
    CU_ASSERT_TRUE(exists);                             \
  } while(0)

#endif /* FILE_ASSERTIONS_H */
