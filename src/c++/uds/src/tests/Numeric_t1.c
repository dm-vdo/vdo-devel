// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/atomic.h>

#include "albtest.h"
#include "assertions.h"
#include "numeric.h"

/*
 * Uses of "buf+1" and such are to exercise access to unaligned data.
 *
 * Calls to smp_mb are to avoid compiler optimizations that may
 * figure out what values were supposed to be stored and short-circuit
 * the actual verification.  (We could also use "volatile" but we'd
 * have to define all the functions as operating on volatile storage.)
 *
 * Don't use automatic variables because the compiler gets some
 * optimization opportunities with them, too.
 */

static const u8 buf[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
static u8 output[16] __attribute__((aligned(16)));

static void getUInt64BETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_be64(buf+1), 0x102030405060708llu);
}

static void getUInt64LETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_le64(buf+1), 0x807060504030201llu);
}

static void storeUInt64BETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_be64(0x102030405060708llu, output+3);
  smp_mb();
  CU_ASSERT_EQUAL(output[2], 'X');
  CU_ASSERT_EQUAL(output[3], 1);
  CU_ASSERT_EQUAL(output[4], 2);
  CU_ASSERT_EQUAL(output[5], 3);
  CU_ASSERT_EQUAL(output[6], 4);
  CU_ASSERT_EQUAL(output[7], 5);
  CU_ASSERT_EQUAL(output[8], 6);
  CU_ASSERT_EQUAL(output[9], 7);
  CU_ASSERT_EQUAL(output[10], 8);
  CU_ASSERT_EQUAL(output[11], 'X');
}

static void storeUInt64LETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_le64(0x102030405060708llu, output+3);
  smp_mb();
  CU_ASSERT_EQUAL(output[2], 'X');
  CU_ASSERT_EQUAL(output[3], 8);
  CU_ASSERT_EQUAL(output[4], 7);
  CU_ASSERT_EQUAL(output[5], 6);
  CU_ASSERT_EQUAL(output[6], 5);
  CU_ASSERT_EQUAL(output[7], 4);
  CU_ASSERT_EQUAL(output[8], 3);
  CU_ASSERT_EQUAL(output[9], 2);
  CU_ASSERT_EQUAL(output[10], 1);
  CU_ASSERT_EQUAL(output[11], 'X');
}

static void getUInt32BETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_be32(buf+1), 0x1020304);
}

static void getUInt32LETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_le32(buf+1), 0x4030201);
}

static void storeUInt32BETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_be32(0x1020304, output+1);
  smp_mb();
  CU_ASSERT_EQUAL(output[0], 'X');
  CU_ASSERT_EQUAL(output[1], 1);
  CU_ASSERT_EQUAL(output[2], 2);
  CU_ASSERT_EQUAL(output[3], 3);
  CU_ASSERT_EQUAL(output[4], 4);
  CU_ASSERT_EQUAL(output[5], 'X');
}

static void storeUInt32LETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_le32(0x1020304, output+1);
  smp_mb();
  CU_ASSERT_EQUAL(output[0], 'X');
  CU_ASSERT_EQUAL(output[1], 4);
  CU_ASSERT_EQUAL(output[2], 3);
  CU_ASSERT_EQUAL(output[3], 2);
  CU_ASSERT_EQUAL(output[4], 1);
  CU_ASSERT_EQUAL(output[5], 'X');
}

static void getUInt16BETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_be16(buf+1), 0x102);
}

static void getUInt16LETest(void)
{
  CU_ASSERT_EQUAL(get_unaligned_le16(buf+1), 0x201);
}

static void storeUInt16BETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_be16(0x102, output+1);
  smp_mb();
  CU_ASSERT_EQUAL(output[0], 'X');
  CU_ASSERT_EQUAL(output[1], 1);
  CU_ASSERT_EQUAL(output[2], 2);
  CU_ASSERT_EQUAL(output[3], 'X');
}

static void storeUInt16LETest(void)
{
  memset(output, 'X', sizeof(output));
  smp_mb();
  put_unaligned_le16(0x102, output+1);
  smp_mb();
  CU_ASSERT_EQUAL(output[0], 'X');
  CU_ASSERT_EQUAL(output[1], 2);
  CU_ASSERT_EQUAL(output[2], 1);
  CU_ASSERT_EQUAL(output[3], 'X');
}

static const CU_TestInfo tests[] = {
  {"GetUInt64BE",     getUInt64BETest    },
  {"GetUInt64LE",     getUInt64LETest    },
  {"StoreUInt64BE",   storeUInt64BETest  },
  {"StoreUInt64LE",   storeUInt64LETest  },
  {"GetUInt32BE",     getUInt32BETest    },
  {"GetUInt32LE",     getUInt32LETest    },
  {"StoreUInt32BE",   storeUInt32BETest  },
  {"StoreUInt32LE",   storeUInt32LETest  },
  {"GetUInt16BE",     getUInt16BETest    },
  {"GetUInt16LE",     getUInt16LETest    },
  {"StoreUInt16BE",   storeUInt16BETest  },
  {"StoreUInt16LE",   storeUInt16LETest  },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name                     = "Numeric_t1",
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
