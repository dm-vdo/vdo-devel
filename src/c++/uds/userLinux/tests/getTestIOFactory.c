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
  int result
    = make_uds_io_factory(getTestIndexName(), FU_READ_WRITE, &factory);
  if (result != UDS_SUCCESS) {
    UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(),
                                           FU_CREATE_READ_WRITE, &factory));
  }
  return factory;
}
