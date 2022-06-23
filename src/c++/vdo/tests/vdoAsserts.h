/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef VDO_ASSERTS_H
#define VDO_ASSERTS_H

#include "status-codes.h"

#include "assertions.h"

#define VDO_ASSERT_SUCCESS(result)                                           \
  do {                                                                       \
    int r_ = (result);                                                       \
    if (r_ != VDO_SUCCESS) {                                                 \
      char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];                               \
      const char *errmsg = TEST_ERROR_NAME_FUNC(r_, errbuf, sizeof(errbuf)); \
      CU_COMPLAIN_AND_DIE("VDO_ASSERT_SUCCESS",                              \
                          "%s: %s (%d)",                                     \
                          #result, errmsg, r_);                              \
    }                                                                        \
  } while(0)

#endif // VDO_ASSERTS_H
