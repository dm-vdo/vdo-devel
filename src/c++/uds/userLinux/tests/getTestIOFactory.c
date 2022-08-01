/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "assertions.h"
#include "testPrototypes.h"

/**********************************************************************/
struct io_factory *getTestIOFactory(void)
{
  struct io_factory *factory;

  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));
  return factory;
}
