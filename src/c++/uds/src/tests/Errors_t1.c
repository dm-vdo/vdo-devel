// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "errors.h"

/**********************************************************************/
static int haveMessage(int code)
{
  char errbuf[VDO_MAX_ERROR_MESSAGE_SIZE];
  const char *message = uds_string_error(code, errbuf, sizeof(errbuf));
  const char *unknown = strstr(message, "Unknown");
  int number;
  return ((unknown == NULL)
          || sscanf(unknown, "Unknown error %d", &number) != 1);
}

/**********************************************************************/
static void udsTest(void)
{
  CU_ASSERT_TRUE(haveMessage(UDS_SUCCESS));

  int code;
  for (code = UDS_ERROR_CODE_BASE; code < UDS_ERROR_CODE_LAST; code++) {
    CU_ASSERT_TRUE(haveMessage(code));
  }
}

/**********************************************************************/
static void blockRegistrationTest(void)
{
  UDS_ASSERT_SUCCESS(uds_register_error_block("foo", 6000, 7000, NULL, 0));
  UDS_ASSERT_SUCCESS(uds_register_error_block("bas", 8000, 9000, NULL, 0));

  UDS_ASSERT_ERROR(UDS_DUPLICATE_NAME,
                   uds_register_error_block("foo", 9001, 9002, NULL, 0));

  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6000, 6001, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6000, 7000, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6001, 7000, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6001, 6999, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6999, 7000, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6999, 7001, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 6999, 7001, NULL, 0));

  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7000, 8001, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7000, 8999, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7000, 9000, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7000, 9001, NULL, 0));
  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7000, 9999, NULL, 0));

  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 5000, 9999, NULL, 0));

  UDS_ASSERT_SUCCESS(uds_register_error_block("baz", 7000, 7500, NULL, 0));

  UDS_ASSERT_ERROR(UDS_ALREADY_REGISTERED,
                   uds_register_error_block("try", 7449, 8000, NULL, 0));

  UDS_ASSERT_SUCCESS(uds_register_error_block("oog", 7500, 7800, NULL, 0));
  UDS_ASSERT_SUCCESS(uds_register_error_block("ook", 7800, 8000, NULL, 0));

  UDS_ASSERT_ERROR(UDS_OVERFLOW,
                   uds_register_error_block("try", 9001, 9002, NULL, 0));
}

/**********************************************************************/

static void errorStringTest(void)
{
  char errbuf[100];

  CU_ASSERT_STRING_EQUAL("Success",
                         uds_string_error(UDS_SUCCESS, errbuf,
                                          sizeof(errbuf)));

  CU_ASSERT_STRING_EQUAL("UDS Error: UDS library context is disabled",
                         uds_string_error(UDS_DISABLED, errbuf,
                                          sizeof(errbuf)));
  CU_ASSERT_STRING_EQUAL("UDS Error: Request queued",
                         uds_string_error(UDS_QUEUED, errbuf, sizeof(errbuf)));

  CU_ASSERT_STRING_EQUAL("Unknown UDS Error 1111",
                         uds_string_error(1111, errbuf, sizeof(errbuf)));
  CU_ASSERT_STRING_EQUAL("Unknown error 999",
                         uds_string_error(999, errbuf, sizeof(errbuf)));
  CU_ASSERT_STRING_EQUAL("Cannot allocate memory",
                         uds_string_error(ENOMEM, errbuf, sizeof(errbuf)));
  CU_ASSERT_STRING_EQUAL("Cannot allocate memory",
                         uds_string_error(-ENOMEM, errbuf, sizeof(errbuf)));
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "UDS Errors",           udsTest               },
  { "Block Registration",   blockRegistrationTest },
  { "Error String",         errorStringTest       },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Errors_t1",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = tests,
};

/**
 * Entry point required by the module loader.
 *
 * @return      a pointer to the const CU_SuiteInfo structure.
 **/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
