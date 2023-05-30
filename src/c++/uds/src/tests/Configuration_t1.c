// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

/**
 * Test the storage size of a UDS configuration
 *
 * We make sure the storage size of the index exactly matches our expectations.
 * If any difference is found, we have found a compatibility problem with
 * reading old indices.
 **/

#include "albtest.h"
#include "assertions.h"
#include "blockTestUtils.h"
#include "config.h"
#include "chapter-index.h"
#include "hash-utils.h"
#include "logger.h"
#include "testPrototypes.h"

/**********************************************************************/
static void sizeCheck(const char               *label,
                      uds_memory_config_size_t  size,
                      uint64_t                  denseSize,
                      uint64_t                  sparseSize)
{
  struct uds_parameters params = {
    .memory_size = size,
  };
  randomizeUdsNonce(&params);

  uint64_t dense;
  UDS_ASSERT_SUCCESS(uds_compute_index_size(&params, &dense));

  uint64_t sparse;
  params.sparse = true;
  UDS_ASSERT_SUCCESS(uds_compute_index_size(&params, &sparse));
   albPrint("%6s  %12llu %13llu", label,
           (unsigned long long) dense,
           (unsigned long long) sparse);
  if (denseSize != sparseSize) {
    CU_ASSERT_EQUAL(denseSize, dense);
    CU_ASSERT_EQUAL(sparseSize, sparse);
  }
}

/**********************************************************************/
static void reducedCheck(const char            *label,
                      uds_memory_config_size_t  confSize,
                      uint64_t                  denseSize,
                      uint64_t                  sparseSize)
{
  /* Modify confSize for reduced chapters/per volume. */
  if (confSize == UDS_MEMORY_CONFIG_256MB) {
    confSize = UDS_MEMORY_CONFIG_REDUCED_256MB;
  } else if (confSize == UDS_MEMORY_CONFIG_512MB) {
    confSize = UDS_MEMORY_CONFIG_REDUCED_512MB;
  } else if (confSize == UDS_MEMORY_CONFIG_768MB) {
    confSize = UDS_MEMORY_CONFIG_REDUCED_768MB;
  } else {
    confSize += UDS_MEMORY_CONFIG_REDUCED;
  }
  sizeCheck(label, confSize, denseSize, sparseSize);
}

/**********************************************************************/
static void sizeTest(void)
{
/*          label    memory size              dense size      sparse size */
  sizeCheck("256MB", UDS_MEMORY_CONFIG_256MB,  2781704192L,   23847940096L);
  sizeCheck("512MB", UDS_MEMORY_CONFIG_512MB,  5596770304L,   48031166464L);
  sizeCheck("768MB", UDS_MEMORY_CONFIG_768MB,  8411828224L,   72214392832L);
  sizeCheck("1GB",   1,                       11193331712L,   96062033920L);
  sizeCheck("2GB",   2,                       22512553984L,  192220721152L);
  sizeCheck("3GB",   3,                       33974505472L,  288426463232L);
  sizeCheck("4GB",   4,                       45293596672L,  384651997184L);
  sizeCheck("5GB",   5,                       56969850880L,  480983830528L);
  sizeCheck("6GB",   6,                       68360376320L,  577177120768L);
  sizeCheck("7GB",   7,                       79750893568L,  673526779904L);
  sizeCheck("8GB",   8,                       91141550080L,  769742483456L);
  sizeCheck("9GB",   9,                      103174713344L,  866269351936L);
  sizeCheck("10GB", 10,                      114636673024L,  962519658496L);
  sizeCheck("11GB", 11,                      126098755584L, 1058769833984L);
  sizeCheck("12GB", 12,                      137560715264L, 1155020140544L);
  sizeCheck("13GB", 13,                      149022666752L, 1251560378368L);
  sizeCheck("14GB", 14,                      160484626432L, 1347832967168L);
  sizeCheck("15GB", 15,                      171946577920L, 1444105555968L);
  sizeCheck("16GB", 16,                      183408537600L, 1540378144768L);
}

/**********************************************************************/
static void reducedSizeTest(void)
{
/*             label    memory size              dense size      sparse size */
  reducedCheck("256MB", UDS_MEMORY_CONFIG_256MB,  2779410432L,   23845646336L);
  reducedCheck("512MB", UDS_MEMORY_CONFIG_512MB,  5592141824L,   48026537984L);
  reducedCheck("768MB", UDS_MEMORY_CONFIG_768MB,  8404881408L,   72207437824L);
  reducedCheck("1GB",   1,                       11184091136L,   96052785152L);
  reducedCheck("2GB",   2,                       22503305216L,  192211472384L);
  reducedCheck("3GB",   3,                       33965264896L,  288417214464L);
  reducedCheck("4GB",   4,                       45284347904L,  384642748416L);
  reducedCheck("5GB",   5,                       56960610304L,  480974581760L);
  reducedCheck("6GB",   6,                       68351127552L,  577167872000L);
  reducedCheck("7GB",   7,                       79741652992L,  673517531136L);
  reducedCheck("8GB",   8,                       91132301312L,  769733234688L);
  reducedCheck("9GB",   9,                      103165472768L,  866260103168L);
  reducedCheck("10GB", 10,                      114627424256L,  962510409728L);
  reducedCheck("11GB", 11,                      126089515008L, 1058760585216L);
  reducedCheck("12GB", 12,                      137551466496L, 1155010891776L);
  reducedCheck("13GB", 13,                      149013426176L, 1251551129600L);
  reducedCheck("14GB", 14,                      160475377664L, 1347823718400L);
  reducedCheck("15GB", 15,                      171937337344L, 1444096307200L);
  reducedCheck("16GB", 16,                      183399288832L, 1540368896000L);
}

/**********************************************************************/

static const CU_TestInfo tests[] = {
  { "Size",         sizeTest },
  { "Reduced Size", reducedSizeTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "Configuration_t1",
  .tests = tests,
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
