/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "asyncVIO.h"

#include "memory-alloc.h"

#include "bio.h"
#include "completion.h"

#include "vdoAsserts.h"
#include "vdoTestBase.h"

/**********************************************************************/
bool logicalIs(struct vdo_completion *completion, logical_block_number_t lbn)
{
  return (isDataVIO(completion)
          && (as_data_vio(completion)->logical.lbn == lbn));
}
