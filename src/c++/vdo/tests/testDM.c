/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/kernel.h>

#include "memory-alloc.h"

#include "status-codes.h"

#include "fileUtils.h"
#include "testDM.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

/* Fake implementations of functions declared in device-mapper.h. */

static struct dm_dev dmDev;

/**********************************************************************/
static void tearDownDM(void)
{
  if (dmDev.bdev->fd != -1) {
    close_file(dmDev.bdev->fd, NULL);
  }

  vdo_free(vdo_forget(dmDev.bdev));
}

/**********************************************************************/
void initializeDM(void)
{
  VDO_ASSERT_SUCCESS(vdo_allocate(1, struct block_device, __func__, &dmDev.bdev));
  dmDev.bdev->fd = -1;
  registerTearDownAction(tearDownDM);
}

/**********************************************************************/
fmode_t dm_table_get_mode(struct dm_table *t __attribute__((unused)))
{
  return 1;
}

/**********************************************************************/
void dm_consume_args(struct dm_arg_set *as, unsigned num_args)
{
	BUG_ON(as->argc < num_args);
	as->argc -= num_args;
	as->argv += num_args;
}

/**********************************************************************/
const char *dm_shift_arg(struct dm_arg_set *as)
{
	char *r;

	if (as->argc) {
		as->argc--;
		r = *as->argv;
		as->argv++;
		return r;
	}

	return NULL;
}

/**********************************************************************/
int dm_get_device(struct dm_target *ti __attribute__((unused)),
                  const char *path,
                  fmode_t mode __attribute__((unused)),
                  struct dm_dev **device)
{
  if (path != NULL) {
    int fd;
    int result;

    if (dmDev.bdev->fd != -1) {
      close_file(dmDev.bdev->fd, NULL);
    }
    
    result = open_file(path, FU_READ_WRITE, &fd);
    if (result != UDS_SUCCESS) {
      return result;
    }

    dmDev.bdev->fd = fd;
  }
  
  *device = &dmDev;
  return VDO_SUCCESS;
}

/**********************************************************************/
void dm_put_device(struct dm_target *ti __attribute__((unused)), struct dm_dev *d)
{
  CU_ASSERT_PTR_EQUAL(d, &dmDev);
}
