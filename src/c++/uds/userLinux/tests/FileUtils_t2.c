/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <sys/stat.h>
#include <unistd.h>

#include "albtest.h"
#include "assertions.h"
#include "fileUtils.h"
#include "memory-alloc.h"

/**********************************************************************/
static void testAbsolutePath(void)
{
  const char *path = "/tmp/file";
  char *absPath;
  UDS_ASSERT_SUCCESS(make_abs_path(path, &absPath));
  CU_ASSERT_STRING_EQUAL(absPath, path);
  uds_free(absPath);
}

/**********************************************************************/
static void testRelativePath(void)
{
  char *savedCwd = get_current_dir_name();
  CU_ASSERT_PTR_NOT_NULL(savedCwd);
  const char *path = "file";
  UDS_ASSERT_SYSTEM_CALL(chdir("/tmp"));
  char *absPath;
  UDS_ASSERT_SUCCESS(make_abs_path(path, &absPath));
  CU_ASSERT_STRING_EQUAL(absPath, "/tmp/file");
  uds_free(absPath);
  UDS_ASSERT_SYSTEM_CALL(chdir(savedCwd));
  uds_free(savedCwd);
}

/**********************************************************************/
static void testBadCWD(void)
{
  char *savedCwd = get_current_dir_name();
  CU_ASSERT_PTR_NOT_NULL(savedCwd);
  const char *dir = "tmp";

  UDS_ASSERT_SYSTEM_CALL(mkdir(dir, 0755));
  UDS_ASSERT_SYSTEM_CALL(chdir(dir));

  char *cwd = get_current_dir_name();
  CU_ASSERT_PTR_NOT_NULL(cwd);
  UDS_ASSERT_SYSTEM_CALL(remove(cwd));
  uds_free(cwd);
  char *path;
  UDS_ASSERT_SUCCESS(uds_duplicate_string("tmp", __func__, &path));
  char *expectedPath = path;
  CU_ASSERT_NOT_EQUAL(make_abs_path(path, &path), UDS_SUCCESS);
  CU_ASSERT_STRING_EQUAL(path, "tmp");
  CU_ASSERT_PTR_EQUAL(path, expectedPath);
  uds_free(path);
  UDS_ASSERT_SYSTEM_CALL(chdir(savedCwd));
  uds_free(savedCwd);
}

/**********************************************************************/
static void testSamePtr(void)
{
  char *savedCwd = get_current_dir_name();
  CU_ASSERT_PTR_NOT_NULL(savedCwd);
  char *path1;
  UDS_ASSERT_SUCCESS(uds_duplicate_string("12345", __func__, &path1));
  char *path2 = path1;
  UDS_ASSERT_SYSTEM_CALL(chdir("/tmp"));
  CU_ASSERT_PTR_EQUAL(path1, path2);
  UDS_ASSERT_SUCCESS(make_abs_path(path1, &path1));
  CU_ASSERT_NOT_EQUAL(path1, path2);
  uds_free(path1);
  uds_free(path2);
  UDS_ASSERT_SYSTEM_CALL(chdir(savedCwd));
  uds_free(savedCwd);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  {"Absolute Path", testAbsolutePath },
  {"Relative Path", testRelativePath },
  {"Same Pointer",  testSamePtr      },
  {"Bad CWD",       testBadCWD       },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "FileUtils_t2",
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
