/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "albtest.h"

#include <linux/atomic.h>
#include <linux/kobject.h>

#include "memory-alloc.h"

#include "vdoAsserts.h"

static bool released[] = { false, false, false, false, };
static struct kobj_type kobjTypes[4];

/**********************************************************************/
static unsigned int toIndex(char id)
{
  return id - 'A';
}

/**********************************************************************/
static void release(struct kobject *kobj, char *name)
{
  CU_ASSERT_STRING_EQUAL(kobj->name, name);
  CU_ASSERT_EQUAL(atomic_read(&(kobj->refcount)), 0);
  released[toIndex(*name)] = true;
  uds_free(kobj);
}

/**********************************************************************/
static void releaseA(struct kobject *kobj)
{
  release(kobj, "A");
}

/**********************************************************************/
static void releaseB(struct kobject *kobj)
{
  release(kobj, "B");
}

/**********************************************************************/
static void releaseC(struct kobject *kobj)
{
  release(kobj, "C");
}

/**********************************************************************/
static void releaseD(struct kobject *kobj)
{
  release(kobj, "D");
}

/**********************************************************************/
static struct kobject *makeKobject(char id, struct kobject *parent)
{
  struct kobject *kobject;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(1, struct kobject, __func__, &kobject));

  unsigned int      index    = toIndex(id);
  struct kobj_type *kobjType = &kobjTypes[index];
  switch (id) {
  case 'A':
    kobjType->release = releaseA;
    break;

  case 'B':
    kobjType->release = releaseB;
    break;

  case 'C':
    kobjType->release = releaseC;
    break;

  case 'D':
    kobjType->release = releaseD;
    break;

  default:
    CU_FAIL("Unknown kobject id");
  }

  kobject_init(kobject, kobjType);
  released[index] = false;
  VDO_ASSERT_SUCCESS(kobject_add(kobject, parent, "%c", id));
  return kobject;
}

/**********************************************************************/
static void assertReleased(char id)
{
  CU_ASSERT_TRUE(released[toIndex(id)]);
}

/**********************************************************************/
static void assertNotReleased(char id)
{
  CU_ASSERT_FALSE(released[toIndex(id)]);
}

/**********************************************************************/
static void assertNoKobjects(void)
{
  CU_ASSERT_EQUAL(0, atomic_read(&kernel_kobj->refcount));
}

/**********************************************************************/
static void testOneKobject(void)
{
  struct kobject *a = makeKobject('A', kernel_kobj);
  assertNotReleased('A');
  kobject_put(a);
  assertReleased('A');
  assertNoKobjects();
}

/**********************************************************************/
static void testSiblingKobjects(void)
{
  struct kobject *a = makeKobject('A', kernel_kobj);
  assertNotReleased('A');

  struct kobject *b = makeKobject('B', kernel_kobj);
  assertNotReleased('A');
  assertNotReleased('B');

  kobject_put(a);
  assertReleased('A');
  assertNotReleased('B');

  kobject_put(b);
  assertReleased('A');
  assertReleased('B');
  assertNoKobjects();
}

/**********************************************************************/
static void testTreeOfKobjects(void)
{
  struct kobject *a = makeKobject('A', kernel_kobj);
  assertNotReleased('A');

  struct kobject *b = makeKobject('B', a);
  assertNotReleased('A');
  assertNotReleased('B');

  struct kobject *c = makeKobject('C', b);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');

  struct kobject *d = makeKobject('D', b);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');
  assertNotReleased('D');

  kobject_put(b);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');
  assertNotReleased('D');

  kobject_get(c);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');
  assertNotReleased('D');

  kobject_put(d);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');
  assertReleased('D');

  kobject_put(c);
  assertNotReleased('A');
  assertNotReleased('B');
  assertNotReleased('C');
  assertReleased('D');

  kobject_put(c);
  assertNotReleased('A');
  assertReleased('B');
  assertReleased('C');
  assertReleased('D');

  kobject_put(a);
  assertReleased('A');
  assertReleased('B');
  assertReleased('C');
  assertReleased('D');

  assertNoKobjects();
}

/**********************************************************************/

static CU_TestInfo tests[] = {
  { "1 kobject",           testOneKobject      },
  { "2 sibling kobjects",  testSiblingKobjects },
  { "tree of kobjects",    testTreeOfKobjects  },
  CU_TEST_INFO_NULL,
};

static CU_SuiteInfo suite = {
  .name        = "Fake kobject tests (Kobject_t1)",
  .initializer = initialize_kernel_kobject,
  .tests = tests
};

CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
